// Clang plugin for tools/audit_raw_access.py: records every raw memory access
// in game/ (a pointer reinterpreted as another type, byte arithmetic on an
// untyped pointer, an integer turned into a pointer, mem* calls) as the
// object it starts from plus a byte offset, so that the same access can be
// resolved against the i386 layout (what the original code meant) and a
// 64-bit layout (what the port touches). It writes JSON lines:
//
//   site    an access: root, constant offset, variable terms, view type, width
//   flow    a value stored into a variable/field/parameter/return value whose
//           target is an untyped pointer or that was computed raw; the script
//           follows these to find what an untyped pointer points at
//   record  the layout of every record type referenced, for this target
//
// Usage: clang++ -fsyntax-only -fplugin=RawAccess.so
//            -fplugin-arg-rawaccess-out=FILE -fplugin-arg-rawaccess-dir=GAMEDIR ...
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <map>
#include <set>

using namespace clang;
namespace json = llvm::json;

namespace {

struct Term {
    std::string text;
    int64_t stride;
};

// The address an expression computes: root + offset + sum(term * stride).
struct Chain {
    std::string kind = "opaque"; // object | pointee | untyped | alloc | abs | opaque
    QualType type;               // object: its type; pointee: the pointed-to type
    std::string prov;            // where the root comes from (variable, field, call...)
    std::string why;             // opaque: what stopped the decomposition
    int64_t offset = 0;
    std::vector<Term> terms;
    bool raw = false;
    std::string punTo; // a pointer to one record reinterpreted as this record
    json::Value size = nullptr; // alloc roots: size in bytes (number) or source text
    std::vector<Term> sizeTerms; // alloc roots: the size as constant + terms
    std::string sizeText;        // alloc roots: the size argument as written
    std::string elementRecord;   // proved from the size expression, not a cast
    int64_t sizeConst = 0;
};

class Analyzer : public RecursiveASTVisitor<Analyzer> {
public:
    Analyzer(ASTContext &ctx, std::string gameDir, std::unique_ptr<llvm::raw_fd_ostream> out)
        : Ctx(ctx), SM(ctx.getSourceManager()), GameDir(std::move(gameDir)), Out(std::move(out)) {}

    void finish()
    {
        std::set<const RecordDecl *> done;
        while (!PendingRecords.empty()) {
            const RecordDecl *rd = *PendingRecords.begin();
            PendingRecords.erase(PendingRecords.begin());
            if (!done.insert(rd).second)
                continue;
            emitRecord(rd);
        }
    }

    bool shouldVisitTemplateInstantiations() const { return false; }

    bool TraverseFunctionDecl(FunctionDecl *fd) { return withFunction(fd, [&] { return RecursiveASTVisitor::TraverseFunctionDecl(fd); }); }
    bool TraverseCXXMethodDecl(CXXMethodDecl *fd) { return withFunction(fd, [&] { return RecursiveASTVisitor::TraverseCXXMethodDecl(fd); }); }
    bool TraverseCXXConstructorDecl(CXXConstructorDecl *fd) { return withFunction(fd, [&] { return RecursiveASTVisitor::TraverseCXXConstructorDecl(fd); }); }
    bool TraverseCXXDestructorDecl(CXXDestructorDecl *fd) { return withFunction(fd, [&] { return RecursiveASTVisitor::TraverseCXXDestructorDecl(fd); }); }

    // Unevaluated operands (sizeof, decltype) access nothing.
    bool TraverseUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *) { return true; }

    bool VisitMemberExpr(MemberExpr *e) { accessLValue(e, "member"); return true; }
    bool VisitArraySubscriptExpr(ArraySubscriptExpr *e) { accessLValue(e, "subscript"); return true; }
    bool VisitUnaryOperator(UnaryOperator *e)
    {
        if (e->getOpcode() == UO_Deref)
            accessLValue(e, "deref");
        return true;
    }

    bool VisitCastExpr(CastExpr *e)
    {
        if (e->getCastKind() != CK_PointerToIntegral || !inGame(e->getExprLoc()))
            return true;
        Chain c;
        decomposePtr(e->getSubExpr(), c);
        json::Object site = siteBase(c, e, "ptr-int");
        site["view"] = nullptr;
        site["width"] = 0;
        site["pointerBits"] = sizeOf(e->getSubExpr()->getType()) * 8;
        site["integerBits"] = sizeOf(e->getType()) * 8;
        emit(std::move(site));
        return true;
    }

    bool VisitVarDecl(VarDecl *vd)
    {
        if (vd->hasInit() && !isa<ParmVarDecl>(vd) && inGame(vd->getLocation()))
            flow(varKey(vd), vd->getType(), vd->getInit(), vd->getLocation(), "init");
        return true;
    }

    bool VisitBinaryOperator(BinaryOperator *e)
    {
        if (e->getOpcode() == BO_Assign && inGame(e->getExprLoc())) {
            std::string dst = lvalueKey(e->getLHS());
            if (!dst.empty())
                flow(dst, e->getLHS()->getType(), e->getRHS(), e->getExprLoc(), "assign");
        }
        return true;
    }

    bool VisitReturnStmt(ReturnStmt *s)
    {
        if (CurFn && s->getRetValue() && inGame(s->getReturnLoc()))
            flow("ret:" + CurFn->getNameAsString(), CurFn->getReturnType(), s->getRetValue(), s->getReturnLoc(), "return");
        return true;
    }

    bool VisitCallExpr(CallExpr *e)
    {
        if (!inGame(e->getExprLoc()))
            return true;
        const FunctionDecl *callee = e->getDirectCallee();
        if (!callee)
            return true;
        std::string name = callee->getNameAsString();
        // Block operations: each pointer argument is accessed over the size argument.
        static const std::map<std::string, std::vector<int>> memArgs = {
            {"memcpy", {0, 1}}, {"memmove", {0, 1}}, {"memcmp", {0, 1}}, {"memset", {0}}};
        auto mem = memArgs.find(name);
        if (mem != memArgs.end() && e->getNumArgs() == 3) {
            Chain width;
            addIntOffset(e->getArg(2), 1, 1, width);
            for (int i : mem->second) {
                Chain c;
                decomposePtr(e->getArg(i), c);
                json::Object site = siteBase(c, e->getArg(i), "mem" + std::to_string(i) + ":" + name);
                site["view"] = nullptr;
                site["width"] = width.offset;
                site["wterms"] = termsJson(width.terms);
                emit(std::move(site));
            }
            return true;
        }
        for (unsigned i = 0; i < e->getNumArgs() && i < callee->getNumParams(); i++)
            flow("param:" + name + "#" + std::to_string(i), callee->getParamDecl(i)->getType(), e->getArg(i),
                 e->getArg(i)->getExprLoc(), "arg");
        return true;
    }

private:
    ASTContext &Ctx;
    SourceManager &SM;
    std::string GameDir;
    std::unique_ptr<llvm::raw_fd_ostream> Out;
    const FunctionDecl *CurFn = nullptr;
    std::set<const RecordDecl *> PendingRecords;
    std::map<std::string, int> KeyCounts;
    std::map<FileID, bool> InGame;

    template <typename F> bool withFunction(FunctionDecl *fd, F traverse)
    {
        const FunctionDecl *saved = CurFn;
        CurFn = fd;
        bool result = traverse();
        CurFn = saved;
        return result;
    }

    // ---- locations and names ------------------------------------------------

    bool inGame(SourceLocation loc)
    {
        loc = SM.getExpansionLoc(loc);
        if (loc.isInvalid())
            return false;
        FileID id = SM.getFileID(loc);
        auto cached = InGame.find(id);
        if (cached != InGame.end())
            return cached->second;
        llvm::SmallString<256> real;
        bool in = !llvm::sys::fs::real_path(SM.getFilename(loc), real) && real.str().starts_with(GameDir);
        InGame[id] = in;
        return in;
    }

    std::string where(SourceLocation loc)
    {
        loc = SM.getExpansionLoc(loc);
        llvm::StringRef file = SM.getFilename(loc);
        size_t slash = file.rfind('/');
        return (slash == llvm::StringRef::npos ? file : file.substr(slash + 1)).str() + ":" +
               std::to_string(SM.getExpansionLineNumber(loc)) + ":" + std::to_string(SM.getExpansionColumnNumber(loc));
    }

    // Keys repeat when a macro expands several accesses at one location;
    // number them in traversal order, which is the same on every target.
    std::string uniqueKey(const std::string &base)
    {
        int n = KeyCounts[base]++;
        return n ? base + "#" + std::to_string(n) : base;
    }

    std::string text(const Expr *e)
    {
        CharSourceRange range = CharSourceRange::getTokenRange(SM.getExpansionRange(e->getSourceRange()).getAsRange());
        std::string s = Lexer::getSourceText(range, SM, Ctx.getLangOpts()).str();
        std::string out;
        bool space = false;
        for (char ch : s) {
            if (ch == '\n' || ch == '\t' || ch == ' ') {
                space = true;
                continue;
            }
            if (space && !out.empty())
                out += ' ';
            space = false;
            out += ch;
        }
        return out.size() > 160 ? out.substr(0, 157) + "..." : out;
    }

    std::string varKey(const VarDecl *vd)
    {
        if (const auto *pd = dyn_cast<ParmVarDecl>(vd)) {
            if (const auto *fd = dyn_cast_or_null<FunctionDecl>(pd->getDeclContext()))
                return "param:" + fd->getNameAsString() + "#" + std::to_string(pd->getFunctionScopeIndex());
        }
        if (vd->isLocalVarDecl() && !vd->isStaticLocal() && CurFn)
            return "var:" + CurFn->getNameAsString() + ":" + vd->getNameAsString() + ":" +
                   std::to_string(SM.getExpansionLineNumber(vd->getLocation()));
        return "var:" + vd->getNameAsString();
    }

    std::string recordKey(const RecordDecl *rd)
    {
        if (!rd->getName().empty())
            return rd->getQualifiedNameAsString();
        if (const TypedefNameDecl *td = rd->getTypedefNameForAnonDecl())
            return td->getNameAsString();
        return "anon@" + where(rd->getLocation());
    }

    // Name of a typed lvalue that holds a value (a variable, a field, an
    // element of either): the destination or source key of a flow. Raw
    // lvalues are named by their access site instead.
    std::string lvalueKey(const Expr *e)
    {
        e = e->IgnoreParenImpCasts();
        if (const auto *as = dyn_cast<ArraySubscriptExpr>(e)) {
            const Expr *base = as->getBase()->IgnoreParenImpCasts();
            if (base->getType()->isArrayType() || isa<DeclRefExpr>(base) || isa<MemberExpr>(base)) {
                Chain c;
                decomposeLValue(as, c);
                if (c.raw)
                    return "site:" + where(as->getExprLoc()) + ":subscript";
                // An element reached through a pointer holds what was stored
                // where that pointer points, not the pointer variable itself.
                if (!base->getType()->isArrayType() && base->getType()->isPointerType())
                    return c.prov.empty() ? "" : "load:" + c.prov;
                return lvalueKey(base);
            }
        }
        if (const auto *dr = dyn_cast<DeclRefExpr>(e)) {
            if (const auto *vd = dyn_cast<VarDecl>(dr->getDecl()))
                return varKey(vd);
            return "";
        }
        if (const auto *me = dyn_cast<MemberExpr>(e)) {
            Chain c;
            decomposeLValue(me, c);
            if (c.raw)
                return "site:" + where(me->getExprLoc()) + ":member";
            if (const auto *fd = dyn_cast<FieldDecl>(me->getMemberDecl()))
                return "field:" + recordKey(fd->getParent()) + "::" + fd->getNameAsString();
            return "";
        }
        if (const auto *uo = dyn_cast<UnaryOperator>(e)) {
            if (uo->getOpcode() == UO_Deref) {
                Chain c;
                decomposeLValue(uo, c);
                if (c.raw)
                    return "site:" + where(uo->getExprLoc()) + ":deref";
                return c.prov.empty() ? "" : "load:" + c.prov;
            }
        }
        return "";
    }

    // ---- types ----------------------------------------------------------------

    static bool isByteLike(QualType t)
    {
        t = t.getCanonicalType().getUnqualifiedType();
        return t->isVoidType() || t->isCharType() || (t->isIntegerType() && !t->isBooleanType() && !t->isEnumeralType() &&
                                                      t->isBuiltinType() && cast<BuiltinType>(t)->getKind() == BuiltinType::UChar);
    }

    int64_t sizeOf(QualType t)
    {
        t = t.getCanonicalType();
        if (t->isVoidType() || t->isIncompleteType() || t->isFunctionType())
            return 1;
        return Ctx.getTypeSizeInChars(t).getQuantity();
    }

    std::string pointeeKey(QualType p)
    {
        p = p.getCanonicalType().getUnqualifiedType();
        if (p->isVoidType())
            return "void";
        if (isByteLike(p))
            return "byte";
        if (const auto *rt = p->getAs<RecordType>()) {
            const RecordDecl *rd = rt->getDecl()->getDefinition();
            if (!rd)
                return "incomplete:" + rt->getDecl()->getNameAsString();
            PendingRecords.insert(rd);
            return "r:" + recordKey(rd);
        }
        if (p->isFunctionType())
            return "fn";
        if (p->isPointerType())
            return "ptr";
        return "s:" + p.getAsString() + ":" + std::to_string(sizeOf(p));
    }

    json::Value typeDesc(QualType t)
    {
        t = t.getCanonicalType().getUnqualifiedType();
        if (t.isNull())
            return nullptr;
        if (const auto *at = Ctx.getAsConstantArrayType(t))
            return json::Object{{"a", typeDesc(at->getElementType())},
                                {"n", (int64_t)at->getSize().getZExtValue()},
                                {"sz", sizeOf(t)}};
        if (const auto *at = Ctx.getAsIncompleteArrayType(t))
            return json::Object{{"a", typeDesc(at->getElementType())}, {"n", 0}, {"sz", 0}};
        if (const auto *rt = t->getAs<RecordType>()) {
            const RecordDecl *rd = rt->getDecl()->getDefinition();
            if (!rd)
                return json::Object{{"s", "incomplete:" + rt->getDecl()->getNameAsString()}, {"sz", 0}};
            PendingRecords.insert(rd);
            return json::Object{{"r", recordKey(rd)}, {"sz", sizeOf(t)}};
        }
        if (t->isPointerType() || t->isReferenceType() || t->isMemberPointerType())
            return json::Object{{"p", pointeeKey(t->getPointeeType())}, {"sz", sizeOf(t)}};
        if (t->isVoidType() || t->isFunctionType())
            return json::Object{{"s", t.getAsString()}, {"sz", 0}};
        return json::Object{{"s", t.getAsString()}, {"sz", sizeOf(t)}};
    }

    void emitRecord(const RecordDecl *rd)
    {
        if (rd->isInvalidDecl() || rd->isDependentType())
            return;
        const ASTRecordLayout &layout = Ctx.getASTRecordLayout(rd);
        json::Array fields;
        if (const auto *cxx = dyn_cast<CXXRecordDecl>(rd)) {
            if (cxx->isDynamicClass() && !layout.getPrimaryBase())
                fields.push_back(json::Object{{"n", "<vptr>"}, {"o", 0}, {"bw", 0},
                                              {"t", json::Object{{"p", "fn"}, {"sz", (int64_t)Ctx.getTypeSizeInChars(Ctx.VoidPtrTy).getQuantity()}}}});
            for (const CXXBaseSpecifier &base : cxx->bases()) {
                const CXXRecordDecl *bd = base.getType()->getAsCXXRecordDecl();
                if (!bd || base.isVirtual())
                    continue;
                fields.push_back(json::Object{{"n", "<" + recordKey(bd) + ">"},
                                              {"o", layout.getBaseClassOffset(bd).getQuantity()},
                                              {"bw", 0},
                                              {"t", typeDesc(base.getType())}});
            }
        }
        unsigned i = 0;
        for (const FieldDecl *fd : rd->fields()) {
            uint64_t bits = layout.getFieldOffset(i++);
            std::string name = fd->getNameAsString();
            if (name.empty())
                name = "<anon" + std::to_string(i - 1) + ">";
            fields.push_back(json::Object{{"n", name},
                                          {"o", (int64_t)(bits / 8)},
                                          {"bw", fd->isBitField() ? (int64_t)fd->getBitWidthValue(Ctx) : 0},
                                          {"t", typeDesc(fd->getType())}});
        }
        emit(json::Object{{"t", "record"},
                          {"n", recordKey(rd)},
                          {"sz", (int64_t)layout.getSize().getQuantity()},
                          {"union", rd->isUnion()},
                          {"fields", std::move(fields)}});
    }

    // ---- decomposition ------------------------------------------------------

    void opaque(Chain &c, const std::string &why)
    {
        c.kind = "opaque";
        c.why = why;
    }

    // The only value a local integer is ever given, when it is a sizeof-based
    // expression (not modified elsewhere, address never taken); else null.
    std::map<const VarDecl *, const Expr *> SingleDefs;

    const Expr *singleDefinition(const VarDecl *vd)
    {
        auto it = SingleDefs.find(vd);
        if (it != SingleDefs.end())
            return it->second;
        SingleDefs[vd] = nullptr;  // guards against recursion through the definition
        const Expr *def = vd->getInit();
        int count = def ? 1 : 0;
        bool escapes = false;
        std::function<void(const Stmt *)> scan = [&](const Stmt *s) {
            if (!s)
                return;
            auto refers = [&](const Expr *x) {
                const auto *dr = dyn_cast<DeclRefExpr>(x->IgnoreParenImpCasts());
                return dr && dr->getDecl() == vd;
            };
            if (const auto *bo = dyn_cast<BinaryOperator>(s)) {
                if (bo->isAssignmentOp() && refers(bo->getLHS())) {
                    count++;
                    if (bo->getOpcode() == BO_Assign)
                        def = bo->getRHS();
                    else
                        escapes = true;
                }
            } else if (const auto *uo = dyn_cast<UnaryOperator>(s)) {
                if ((uo->isIncrementDecrementOp() || uo->getOpcode() == UO_AddrOf) && refers(uo->getSubExpr()))
                    escapes = true;
            }
            for (const Stmt *child : s->children())
                scan(child);
        };
        if (const auto *fd = dyn_cast_or_null<FunctionDecl>(vd->getParentFunctionOrMethod()))
            scan(fd->getBody());
        const Expr *result = !escapes && count == 1 && def && containsSizeof(def) ? def : nullptr;
        SingleDefs[vd] = result;
        return result;
    }

    // Integer expression scaled into the chain's offset: constants fold, the
    // rest become terms with their byte stride.
    void addIntOffset(const Expr *e, int64_t scale, int64_t sign, Chain &c)
    {
        e = e->IgnoreParenImpCasts();
        if (auto v = e->getIntegerConstantExpr(Ctx)) {
            c.offset += sign * v->getExtValue() * scale;
            return;
        }
        if (const auto *bo = dyn_cast<BinaryOperator>(e)) {
            switch (bo->getOpcode()) {
            case BO_Add:
                addIntOffset(bo->getLHS(), scale, sign, c);
                addIntOffset(bo->getRHS(), scale, sign, c);
                return;
            case BO_Sub:
                addIntOffset(bo->getLHS(), scale, sign, c);
                addIntOffset(bo->getRHS(), scale, -sign, c);
                return;
            case BO_Mul:
                if (auto v = bo->getRHS()->IgnoreParenImpCasts()->getIntegerConstantExpr(Ctx)) {
                    c.terms.push_back({text(bo->getLHS()), sign * scale * v->getExtValue()});
                    return;
                }
                if (auto v = bo->getLHS()->IgnoreParenImpCasts()->getIntegerConstantExpr(Ctx)) {
                    c.terms.push_back({text(bo->getRHS()), sign * scale * v->getExtValue()});
                    return;
                }
                break;
            case BO_Shl:
                if (auto v = bo->getRHS()->IgnoreParenImpCasts()->getIntegerConstantExpr(Ctx)) {
                    c.terms.push_back({text(bo->getLHS()), sign * (scale << v->getExtValue())});
                    return;
                }
                break;
            default:
                break;
            }
        }
        // A byte cursor computed once as index * sizeof(T): its stride.
        if (const auto *dr = dyn_cast<DeclRefExpr>(e))
            if (const auto *vd = dyn_cast<VarDecl>(dr->getDecl()))
                if (vd->isLocalVarDecl() && vd->getType()->isIntegerType())
                    if (const Expr *def = singleDefinition(vd)) {
                        addIntOffset(def, scale, sign, c);
                        return;
                    }
        c.terms.push_back({text(e), sign * scale});
    }

    // The value of a pointer-holding lvalue: a pointee root whose provenance is
    // the lvalue (variable, field, or the raw access that loaded it).
    void leafPointer(const Expr *lv, QualType pointerType, Chain &c)
    {
        lv = lv->IgnoreParens();
        QualType pointee = pointerType->isPointerType() ? pointerType->getPointeeType() : QualType();
        if (pointee.isNull() || isByteLike(pointee)) {
            c.kind = "untyped";
            c.type = pointee;
        } else {
            c.kind = "pointee";
            c.type = pointee;
        }
        c.prov = lvalueKey(lv);
        if (const auto *th = dyn_cast<CXXThisExpr>(lv))
            c.prov = "this";
        if (c.prov.empty())
            c.prov = "expr:" + text(lv);
    }

    bool containsSizeof(const Stmt *s)
    {
        if (const auto *u = dyn_cast<UnaryExprOrTypeTraitExpr>(s))
            if (u->getKind() == UETT_SizeOf)
                return true;
        for (const Stmt *child : s->children())
            if (child && containsSizeof(child))
                return true;
        return false;
    }

    // Only sizeof(record), a product with a count, and optional constant
    // trailing padding describe a homogeneous record allocation. Merely
    // finding one sizeof in a mixed arena's expression proves nothing.
    std::string allocationRecord(const Expr *e)
    {
        e = e->IgnoreParenImpCasts();
        if (const auto *u = dyn_cast<UnaryExprOrTypeTraitExpr>(e)) {
            if (u->getKind() == UETT_SizeOf) {
                QualType t = u->getTypeOfArgument();
                if (const RecordDecl *rd = t->getAsRecordDecl()) {
                    PendingRecords.insert(rd);
                    return recordKey(rd);
                }
            }
            return "";
        }
        const auto *b = dyn_cast<BinaryOperator>(e);
        if (!b)
            return "";
        for (int reverse = 0; reverse < 2; ++reverse) {
            const Expr *records = reverse ? b->getRHS() : b->getLHS();
            const Expr *other = reverse ? b->getLHS() : b->getRHS();
            std::string name = allocationRecord(records);
            if (name.empty() || containsSizeof(other))
                continue;
            if (b->getOpcode() == BO_Mul)
                return name;
            if (b->getOpcode() == BO_Add) {
                auto n = other->getIntegerConstantExpr(Ctx);
                if (n && !n->isNegative())
                    return name;
            }
        }
        return "";
    }

    void callLeaf(const CallExpr *call, Chain &c)
    {
        const FunctionDecl *callee = call->getDirectCallee();
        std::string name = callee ? callee->getNameAsString() : "";
        static const std::map<std::string, std::vector<int>> allocators = {
            {"AllocateLockedBuffer", {0}}, {"malloc", {0}}, {"farmalloc", {0}},
            {"calloc", {0, 1}}, {"ReallocateLockedBuffer", {1}}, {"realloc", {1}}};
        auto alloc = allocators.find(name);
        if (alloc != allocators.end()) {
            c.kind = "alloc";
            c.prov = "alloc:" + where(call->getExprLoc());
            int64_t size = 1;
            std::string sizeText;
            std::vector<const Expr *> variable;
            bool ambiguousRecord = false;
            for (int i : alloc->second) {
                if (i >= (int)call->getNumArgs())
                    continue;
                const Expr *arg = call->getArg(i);
                c.sizeText += (c.sizeText.empty() ? "" : " * ") + ("(" + text(arg) + ")");
                std::string record = allocationRecord(arg);
                if (!record.empty()) {
                    if (!c.elementRecord.empty())
                        ambiguousRecord = true;
                    c.elementRecord = record;
                } else if (containsSizeof(arg)) {
                    ambiguousRecord = true;
                }
                if (auto v = arg->IgnoreParenImpCasts()->getIntegerConstantExpr(Ctx))
                    size *= v->getExtValue();
                else {
                    sizeText += (sizeText.empty() ? "" : " * ") + text(arg);
                    variable.push_back(arg);
                }
            }
            if (ambiguousRecord)
                c.elementRecord.clear();
            if (sizeText.empty())
                c.size = size;
            else
                c.size = (size == 1 ? "" : std::to_string(size) + " * ") + sizeText;
            if (variable.size() == 1) {
                Chain sz;
                addIntOffset(variable[0], size, 1, sz);
                c.sizeConst = sz.offset;
                c.sizeTerms = sz.terms;
            }
            return;
        }
        QualType t = call->getType();
        c.kind = t->isPointerType() && !isByteLike(t->getPointeeType()) ? "pointee" : "untyped";
        c.type = t->isPointerType() ? t->getPointeeType() : QualType();
        c.prov = callee ? "ret:" + name : "ret:?" + text(call->getCallee());
    }

    void decomposePtr(const Expr *e, Chain &c)
    {
        e = e->IgnoreParens();
        if (const auto *ice = dyn_cast<ImplicitCastExpr>(e)) {
            switch (ice->getCastKind()) {
            case CK_ArrayToPointerDecay:
                decomposeLValue(ice->getSubExpr(), c);
                return;
            case CK_LValueToRValue:
                leafPointer(ice->getSubExpr(), ice->getType(), c);
                return;
            case CK_NoOp:
            case CK_BitCast:
            case CK_DerivedToBase:
            case CK_UncheckedDerivedToBase:
                decomposePtr(ice->getSubExpr(), c);
                return;
            case CK_IntegralToPointer:
                c.raw = true;
                decomposeInt(ice->getSubExpr(), c);
                return;
            case CK_NullToPointer:
                opaque(c, "null");
                return;
            default:
                opaque(c, ice->getCastKindName());
                return;
            }
        }
        if (const auto *ce = dyn_cast<ExplicitCastExpr>(e)) {
            switch (ce->getCastKind()) {
            case CK_BitCast: {
                QualType from = ce->getSubExpr()->getType(), to = ce->getType();
                if (from->isPointerType() && to->isPointerType()) {
                    QualType f = from->getPointeeType().getCanonicalType().getUnqualifiedType();
                    QualType t = to->getPointeeType().getCanonicalType().getUnqualifiedType();
                    if (f != t && !t->isVoidType())
                        c.raw = true;
                    if (f != t && f->isRecordType() && t->isRecordType() && c.punTo.empty())
                        c.punTo = recordKey(t->getAsRecordDecl());
                }
                decomposePtr(ce->getSubExpr(), c);
                return;
            }
            case CK_NoOp:
            case CK_DerivedToBase:
            case CK_UncheckedDerivedToBase:
            case CK_BaseToDerived:
                decomposePtr(ce->getSubExpr(), c);
                return;
            case CK_IntegralToPointer:
                c.raw = true;
                decomposeInt(ce->getSubExpr(), c);
                return;
            case CK_NullToPointer:
                opaque(c, "null");
                return;
            default:
                opaque(c, ce->getCastKindName());
                return;
            }
        }
        if (const auto *bo = dyn_cast<BinaryOperator>(e)) {
            if ((bo->getOpcode() == BO_Add || bo->getOpcode() == BO_Sub) && bo->getType()->isPointerType()) {
                const Expr *ptr = bo->getLHS(), *idx = bo->getRHS();
                if (!ptr->getType()->isPointerType())
                    std::swap(ptr, idx);
                QualType pointee = ptr->getType()->getPointeeType();
                decomposePtr(ptr, c);
                if (isByteLike(pointee) && !byteArrayObject(c))
                    c.raw = true;
                addIntOffset(idx, sizeOf(pointee), bo->getOpcode() == BO_Sub ? -1 : 1, c);
                return;
            }
            if (bo->getOpcode() == BO_Comma) {
                decomposePtr(bo->getRHS(), c);
                return;
            }
            if (bo->isAssignmentOp()) {
                decomposePtr(bo->getLHS(), c);
                return;
            }
        }
        if (const auto *uo = dyn_cast<UnaryOperator>(e)) {
            if (uo->getOpcode() == UO_AddrOf) {
                decomposeLValue(uo->getSubExpr(), c);
                return;
            }
        }
        if (const auto *call = dyn_cast<CallExpr>(e)) {
            callLeaf(call, c);
            return;
        }
        if (isa<CXXThisExpr>(e)) {
            c.kind = "pointee";
            c.type = e->getType()->getPointeeType();
            c.prov = "this";
            return;
        }
        if (isa<StringLiteral>(e)) {
            c.kind = "blob";
            c.prov = "string";
            return;
        }
        if (const auto *ne = dyn_cast<CXXNewExpr>(e)) {
            c.kind = "alloc";
            c.prov = "alloc:" + where(ne->getExprLoc());
            int64_t elem = sizeOf(ne->getAllocatedType());
            if (!ne->isArray())
                c.size = elem;
            else if (auto n = (*ne->getArraySize())->IgnoreParenImpCasts()->getIntegerConstantExpr(Ctx))
                c.size = n->getExtValue() * elem;
            else {
                c.size = text(*ne->getArraySize()) + " * " + std::to_string(elem);
                Chain sz;
                addIntOffset(*ne->getArraySize(), elem, 1, sz);
                c.sizeConst = sz.offset;
                c.sizeTerms = sz.terms;
            }
            return;
        }
        opaque(c, e->getStmtClassName());
    }

    bool byteArrayObject(const Chain &c)
    {
        if (c.kind != "object" || c.type.isNull())
            return false;
        QualType t = c.type.getCanonicalType();
        while (const ArrayType *at = Ctx.getAsArrayType(t))
            t = at->getElementType().getCanonicalType();
        return isByteLike(t);
    }

    // Integer expressions that hold addresses (the operand of a cast to a pointer).
    void decomposeInt(const Expr *e, Chain &c)
    {
        e = e->IgnoreParens();
        if (const auto *ce = dyn_cast<CastExpr>(e)) {
            switch (ce->getCastKind()) {
            case CK_PointerToIntegral:
                c.raw = true;
                decomposePtr(ce->getSubExpr(), c);
                return;
            case CK_IntegralCast:
            case CK_NoOp:
                decomposeInt(ce->getSubExpr(), c);
                return;
            case CK_LValueToRValue: {
                c.kind = "untyped";
                c.prov = lvalueKey(ce->getSubExpr());
                if (c.prov.empty())
                    c.prov = "expr:" + text(ce->getSubExpr());
                return;
            }
            default:
                break;
            }
        }
        if (const auto *bo = dyn_cast<BinaryOperator>(e)) {
            if (bo->getOpcode() == BO_Add || bo->getOpcode() == BO_Sub) {
                const Expr *addr = bo->getLHS(), *off = bo->getRHS();
                if (bo->getOpcode() == BO_Add && !holdsAddress(addr) && holdsAddress(off))
                    std::swap(addr, off);
                decomposeInt(addr, c);
                addIntOffset(off, 1, bo->getOpcode() == BO_Sub ? -1 : 1, c);
                return;
            }
        }
        if (auto v = e->getIntegerConstantExpr(Ctx)) {
            c.kind = "abs";
            c.offset += v->getExtValue();
            return;
        }
        if (const auto *call = dyn_cast<CallExpr>(e)) {
            callLeaf(call, c);
            return;
        }
        opaque(c, e->getStmtClassName());
    }

    // Whether an integer operand is the address side of an addition.
    bool holdsAddress(const Expr *e)
    {
        e = e->IgnoreParenImpCasts();
        if (e->getIntegerConstantExpr(Ctx))
            return false;
        if (const auto *bo = dyn_cast<BinaryOperator>(e))
            return bo->getOpcode() != BO_Mul && bo->getOpcode() != BO_Shl &&
                   (holdsAddress(bo->getLHS()) || holdsAddress(bo->getRHS()));
        if (const auto *ce = dyn_cast<CastExpr>(e->IgnoreParens()))
            return ce->getCastKind() == CK_PointerToIntegral;
        return true;
    }

    // The address of an lvalue.
    void decomposeLValue(const Expr *e, Chain &c)
    {
        e = e->IgnoreParens();
        if (const auto *dr = dyn_cast<DeclRefExpr>(e)) {
            if (const auto *vd = dyn_cast<VarDecl>(dr->getDecl())) {
                if (vd->getType()->isReferenceType()) {
                    c.kind = "pointee";
                    c.type = vd->getType()->getPointeeType();
                    c.prov = varKey(vd);
                    return;
                }
                c.kind = "object";
                c.type = vd->getType();
                c.prov = varKey(vd);
                return;
            }
            opaque(c, "declref");
            return;
        }
        if (const auto *me = dyn_cast<MemberExpr>(e)) {
            if (me->isArrow())
                decomposePtr(me->getBase(), c);
            else
                decomposeLValue(me->getBase(), c);
            if (isa<FieldDecl>(me->getMemberDecl()) || isa<IndirectFieldDecl>(me->getMemberDecl()))
                c.offset += Ctx.getFieldOffset(me->getMemberDecl()) / 8;
            else
                opaque(c, "member");
            return;
        }
        if (const auto *as = dyn_cast<ArraySubscriptExpr>(e)) {
            QualType elem = as->getType();
            decomposePtr(as->getBase(), c);
            if (isByteLike(elem) && !byteArrayObject(c) && c.kind != "opaque")
                c.raw = true;
            addIntOffset(as->getIdx(), sizeOf(elem), 1, c);
            return;
        }
        if (const auto *uo = dyn_cast<UnaryOperator>(e)) {
            if (uo->getOpcode() == UO_Deref) {
                decomposePtr(uo->getSubExpr(), c);
                return;
            }
        }
        if (const auto *ce = dyn_cast<CastExpr>(e)) {
            if (ce->getCastKind() == CK_NoOp || ce->getCastKind() == CK_LValueBitCast) {
                if (ce->getCastKind() == CK_LValueBitCast)
                    c.raw = true;
                decomposeLValue(ce->getSubExpr(), c);
                return;
            }
        }
        opaque(c, e->getStmtClassName());
    }

    // ---- output -------------------------------------------------------------

    json::Array termsJson(const std::vector<Term> &terms)
    {
        json::Array out;
        for (const Term &t : terms)
            out.push_back(json::Array{t.text, t.stride});
        return out;
    }

    json::Object siteBase(const Chain &c, const Expr *e, const std::string &kind)
    {
        json::Object root{{"k", c.kind}, {"prov", c.prov}};
        if (!c.type.isNull())
            root["type"] = typeDesc(c.type);
        if (c.kind == "opaque")
            root["why"] = c.why;
        if (c.kind == "alloc") {
            root["size"] = c.size;
            root["sizeConst"] = c.sizeConst;
            root["sizeTerms"] = termsJson(c.sizeTerms);
            root["sizeText"] = c.sizeText;
            root["elementRecord"] = c.elementRecord;
        }
        return json::Object{{"t", "site"},
                            {"key", uniqueKey(where(e->getExprLoc()) + ":" + kind)},
                            {"fn", CurFn ? CurFn->getNameAsString() : ""},
                            {"raw", c.raw},
                            {"pun", c.punTo},
                            {"root", std::move(root)},
                            {"off", c.offset},
                            {"terms", termsJson(c.terms)},
                            {"text", text(e)}};
    }

    // A raw lvalue that is not the base of a larger one: one access.
    void accessLValue(const Expr *e, const char *kind)
    {
        if (!inGame(e->getExprLoc()) || !isMaximal(e))
            return;
        Chain c;
        decomposeLValue(e, c);
        if (!c.raw)
            return;
        json::Object site = siteBase(c, e, kind);
        site["view"] = typeDesc(e->getType());
        site["width"] = sizeOf(e->getType());
        emit(std::move(site));
    }

    bool isMaximal(const Expr *e)
    {
        const Expr *child = e;
        for (;;) {
            auto parents = Ctx.getParents(*child);
            if (parents.empty())
                return true;
            const Expr *parent = parents[0].get<Expr>();
            if (!parent)
                return true;
            if (isa<ParenExpr>(parent)) {
                child = parent;
                continue;
            }
            if (const auto *me = dyn_cast<MemberExpr>(parent))
                return !(!me->isArrow() && me->getBase()->IgnoreParens() == child);
            if (const auto *ice = dyn_cast<ImplicitCastExpr>(parent))
                return ice->getCastKind() != CK_ArrayToPointerDecay;
            if (const auto *uo = dyn_cast<UnaryOperator>(parent))
                return uo->getOpcode() != UO_AddrOf;
            if (isa<UnaryExprOrTypeTraitExpr>(parent))
                return false;
            return true;
        }
    }

    // A value stored somewhere. Emitted when the destination is an untyped
    // pointer or an integer (so the script can follow what it points at), or
    // when the value was computed raw (then it is also checked as an access
    // of the destination's pointee).
    void flow(const std::string &dst, QualType dstType, const Expr *value, SourceLocation loc, const char *kind)
    {
        dstType = dstType.getCanonicalType();
        bool pointer = dstType->isPointerType();
        bool integer = dstType->isIntegerType() && !dstType->isBooleanType() && sizeOf(dstType) >= 4;
        if (!pointer && !integer)
            return;
        const Expr *v = value->IgnoreParens();
        if (const auto *co = dyn_cast<ConditionalOperator>(value->IgnoreParenImpCasts())) {
            flow(dst, dstType, co->getTrueExpr(), loc, kind);
            flow(dst, dstType, co->getFalseExpr(), loc, kind);
            return;
        }
        Chain c;
        if (pointer || value->getType()->isPointerType())
            decomposePtr(value, c);
        else
            decomposeInt(value, c);
        // Pointers to records carry their type; pointers to bytes or scalars may
        // point into anything, so record where they come from.
        // Typed pointers are recorded too: a reinterpreted record pointer is
        // traced back to the object it really points at.
        if (c.kind == "opaque" && (c.why == "null" || c.why == "FunctionToPointerDecay" || c.why == "Dependent"))
            return;
        if (!pointer && !c.raw && c.kind == "opaque")
            return;
        if (!pointer && !c.raw && c.kind == "untyped" && c.prov.rfind("var:", 0) != 0 && c.prov.rfind("param:", 0) != 0 &&
            c.prov.rfind("field:", 0) != 0)
            return;
        json::Object site = siteBase(c, v, std::string("flow:") + kind);
        site["t"] = "flow";
        site["dst"] = dst;
        if (pointer && !dstType->getPointeeType()->isVoidType() && !dstType->getPointeeType()->isIncompleteType() &&
            !dstType->getPointeeType()->isFunctionType()) {
            site["view"] = typeDesc(dstType->getPointeeType());
            site["width"] = isByteLike(dstType->getPointeeType()) ? 0 : sizeOf(dstType->getPointeeType());
        } else {
            site["view"] = nullptr;
            site["width"] = 0;
        }
        site["int"] = !pointer;
        emit(std::move(site));
    }

    void emit(json::Object obj)
    {
        *Out << json::Value(std::move(obj)) << "\n";
    }
};

class Consumer : public ASTConsumer {
public:
    Consumer(std::string gameDir, std::string out) : GameDir(std::move(gameDir)), OutPath(std::move(out)) {}

    void HandleTranslationUnit(ASTContext &ctx) override
    {
        std::error_code ec;
        auto out = std::make_unique<llvm::raw_fd_ostream>(OutPath.empty() ? "-" : OutPath, ec);
        if (ec) {
            llvm::errs() << "rawaccess: cannot write " << OutPath << ": " << ec.message() << "\n";
            return;
        }
        Analyzer analyzer(ctx, GameDir, std::move(out));
        analyzer.TraverseDecl(ctx.getTranslationUnitDecl());
        analyzer.finish();
    }

private:
    std::string GameDir, OutPath;
};

class Action : public PluginASTAction {
protected:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &, llvm::StringRef) override
    {
        return std::make_unique<Consumer>(GameDir, OutPath);
    }

    bool ParseArgs(const CompilerInstance &, const std::vector<std::string> &args) override
    {
        for (const std::string &arg : args) {
            if (arg.rfind("out=", 0) == 0)
                OutPath = arg.substr(4);
            else if (arg.rfind("dir=", 0) == 0)
                GameDir = arg.substr(4);
        }
        return true;
    }

    ActionType getActionType() override { return AddAfterMainAction; }

private:
    std::string GameDir, OutPath;
};

} // namespace

static FrontendPluginRegistry::Add<Action> X("rawaccess", "record raw memory accesses for tools/audit_raw_access.py");
