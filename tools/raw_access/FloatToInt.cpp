// Clang plugin for tools/ftoi_rewrite.py: lists every floating-point to
// integer conversion in the game source so that it can be written as
// FTOI(type, value). The original builds most translation units with /QIfist,
// where such a conversion is an x87 FISTP that rounds to nearest; FTOI keeps
// MSVC6's code and lets other compilers round the same way.
//
// Writes JSON lines:
//   conv     a conversion to rewrite: the source range of the converted value,
//            the cast prefix to replace (explicit casts) and the target type
//   skip     a conversion that cannot be rewritten in place (inside a macro
//            body, a constant expression, a compound assignment)
//
// Usage: clang++ -fsyntax-only -fplugin=FloatToInt.so
//            -fplugin-arg-ftoi-out=FILE -fplugin-arg-ftoi-dir=SOURCEDIR ...
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <map>

using namespace clang;
namespace json = llvm::json;

namespace {

class Finder : public RecursiveASTVisitor<Finder> {
public:
    Finder(ASTContext &ctx, std::string dir, llvm::raw_ostream &out)
        : Ctx(ctx), SM(ctx.getSourceManager()), Dir(std::move(dir)), Out(out) {}

    bool shouldVisitTemplateInstantiations() const { return false; }

    bool TraverseFunctionDecl(FunctionDecl *fd) { return inFunction([&] { return RecursiveASTVisitor::TraverseFunctionDecl(fd); }); }
    bool TraverseCXXMethodDecl(CXXMethodDecl *fd) { return inFunction([&] { return RecursiveASTVisitor::TraverseCXXMethodDecl(fd); }); }
    bool TraverseCXXConstructorDecl(CXXConstructorDecl *fd) { return inFunction([&] { return RecursiveASTVisitor::TraverseCXXConstructorDecl(fd); }); }
    bool TraverseCXXDestructorDecl(CXXDestructorDecl *fd) { return inFunction([&] { return RecursiveASTVisitor::TraverseCXXDestructorDecl(fd); }); }
    bool TraverseUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *) { return true; }

    bool VisitCastExpr(CastExpr *e)
    {
        if (e->getCastKind() != CK_FloatingToIntegral)
            return true;
        const Expr *value = e->getSubExpr();
        SourceLocation where = e->getExprLoc();
        if (!inDir(where))
            return true;
        if (e->getType()->isBooleanType())
            return true;
        std::string type;
        SourceLocation prefixBegin, prefixEnd;
        // An explicit cast may wrap the conversion as its implicit child.
        const CStyleCastExpr *cs = dyn_cast<CStyleCastExpr>(e);
        if (!cs && isa<ImplicitCastExpr>(e)) {
            auto parents = Ctx.getParents(*e);
            if (!parents.empty())
                cs = parents[0].get<CStyleCastExpr>();
            if (cs && cs->getCastKind() != CK_NoOp)
                cs = nullptr;
        }
        // FTOI's own cast is spelled in FloatToInt.h.
        if (llvm::StringRef(SM.getFilename(SM.getSpellingLoc(cs ? cs->getLParenLoc() : e->getBeginLoc())))
                .ends_with("FloatToInt.h"))
            return true;
        if (cs) {
            prefixBegin = fileLoc(cs->getLParenLoc());
            prefixEnd = fileLoc(cs->getRParenLoc());
            if (prefixBegin.isInvalid() || prefixEnd.isInvalid())
                return skip(e, "cast inside a macro body");
            type = text(SourceRange(prefixBegin.getLocWithOffset(1), prefixEnd.getLocWithOffset(-1)));
            if (type.empty())
                return skip(e, "cast type not found in the file");
        } else if (isa<ImplicitCastExpr>(e)) {
            type = e->getType().getAsString(Ctx.getPrintingPolicy());
        } else {
            return skip(e, "cast form " + std::string(e->getStmtClassName()));
        }
        if (value->isValueDependent() || e->isIntegerConstantExpr(Ctx) || inConstantContext(e))
            return skip(e, "constant expression");
        // The value's own range, in the file.
        SourceLocation vb = fileLoc(value->getBeginLoc()), ve = fileLoc(value->getEndLoc());
        if (vb.isInvalid() || ve.isInvalid())
            return skip(e, "inside a macro body");
        if (prefixBegin.isValid()) {
            prefixBegin = fileLoc(prefixBegin);
            prefixEnd = fileLoc(prefixEnd);
            if (prefixBegin.isInvalid() || prefixEnd.isInvalid())
                return skip(e, "cast inside a macro body");
        }
        SourceLocation vend = Lexer::getLocForEndOfToken(ve, 0, SM, Ctx.getLangOpts());
        if (SM.getFileID(vb) != SM.getFileID(vend))
            return skip(e, "value spans files");
        json::Object row{{"t", "conv"},
                         {"file", realPath(vb)},
                         {"line", (int64_t)SM.getSpellingLineNumber(vb)},
                         {"type", type},
                         {"begin", (int64_t)SM.getFileOffset(vb)},
                         {"end", (int64_t)SM.getFileOffset(vend)},
                         {"explicit", prefixBegin.isValid()}};
        if (prefixBegin.isValid()) {
            row["prefix"] = (int64_t)SM.getFileOffset(prefixBegin);
            row["prefixEnd"] = (int64_t)SM.getFileOffset(prefixEnd) + 1;
        }
        emit(std::move(row));
        return true;
    }

    // `i += f` converts the float result back without a cast node.
    bool VisitCompoundAssignOperator(CompoundAssignOperator *e)
    {
        if (!inDir(e->getExprLoc()))
            return true;
        if (e->getType()->isIntegerType() && e->getComputationResultType()->isRealFloatingType())
            skip(e, "compound assignment of an integer from a float");
        return true;
    }

private:
    ASTContext &Ctx;
    SourceManager &SM;
    std::string Dir;
    llvm::raw_ostream &Out;
    std::map<FileID, std::string> Paths;
    int Functions = 0;

    template <typename F> bool inFunction(F traverse)
    {
        Functions++;
        bool result = traverse();
        Functions--;
        return result;
    }

    std::string realPath(SourceLocation loc)
    {
        FileID id = SM.getFileID(loc);
        auto it = Paths.find(id);
        if (it != Paths.end())
            return it->second;
        llvm::SmallString<256> real;
        if (llvm::sys::fs::real_path(SM.getFilename(loc), real))
            real = SM.getFilename(loc);
        return Paths[id] = real.str().str();
    }

    bool inDir(SourceLocation loc)
    {
        loc = SM.getExpansionLoc(loc);
        return loc.isValid() && llvm::StringRef(realPath(loc)).starts_with(Dir);
    }

    // A location written in the file (directly or as a macro argument).
    SourceLocation fileLoc(SourceLocation loc)
    {
        if (loc.isFileID())
            return loc;
        if (SM.isMacroArgExpansion(loc)) {
            SourceLocation spelling = SM.getSpellingLoc(loc);
            if (spelling.isFileID())
                return spelling;
        }
        return SourceLocation();
    }

    // Global initialisers are folded by the compiler, not converted at run time.
    bool inConstantContext(const Expr *) { return Functions == 0; }

    std::string text(SourceRange range)
    {
        CharSourceRange r = CharSourceRange::getCharRange(range.getBegin(), range.getEnd().getLocWithOffset(1));
        return Lexer::getSourceText(r, SM, Ctx.getLangOpts()).str();
    }

    bool skip(const Expr *e, const std::string &why)
    {
        SourceLocation loc = SM.getExpansionLoc(e->getExprLoc());
        std::string macro;
        if (e->getExprLoc().isMacroID())
            macro = Lexer::getImmediateMacroName(e->getExprLoc(), SM, Ctx.getLangOpts()).str();
        if (macro == "FTOI")
            return true;
        emit(json::Object{{"t", "skip"},
                          {"file", realPath(loc)},
                          {"line", (int64_t)SM.getExpansionLineNumber(loc)},
                          {"why", why},
                          {"macro", macro}});
        return true;
    }

    void emit(json::Object obj) { Out << json::Value(std::move(obj)) << "\n"; }
};

class Consumer : public ASTConsumer {
public:
    Consumer(std::string dir, std::string out) : Dir(std::move(dir)), OutPath(std::move(out)) {}

    void HandleTranslationUnit(ASTContext &ctx) override
    {
        std::error_code ec;
        llvm::raw_fd_ostream out(OutPath.empty() ? "-" : OutPath, ec);
        if (ec) {
            llvm::errs() << "ftoi: cannot write " << OutPath << ": " << ec.message() << "\n";
            return;
        }
        Finder finder(ctx, Dir, out);
        finder.TraverseDecl(ctx.getTranslationUnitDecl());
    }

private:
    std::string Dir, OutPath;
};

class Action : public PluginASTAction {
protected:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &, llvm::StringRef) override
    {
        return std::make_unique<Consumer>(Dir, OutPath);
    }

    bool ParseArgs(const CompilerInstance &, const std::vector<std::string> &args) override
    {
        for (const std::string &arg : args) {
            if (arg.rfind("out=", 0) == 0)
                OutPath = arg.substr(4);
            else if (arg.rfind("dir=", 0) == 0)
                Dir = arg.substr(4);
        }
        return true;
    }

    ActionType getActionType() override { return AddAfterMainAction; }

private:
    std::string Dir, OutPath;
};

} // namespace

static FrontendPluginRegistry::Add<Action> X("ftoi", "list float to integer conversions for tools/ftoi_rewrite.py");
