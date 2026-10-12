package io.github.pablocpas.opencmr2

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.os.StatFs
import android.provider.DocumentsContract
import android.provider.DocumentsContract.Document
import kotlinx.coroutines.ensureActive
import java.io.File
import java.io.IOException
import kotlin.coroutines.coroutineContext

/**
 * The copy of the PC game the app plays: an installation copied into the
 * app's storage, read by the game with ordinary file access. Android gives
 * apps no such access to a folder the player chooses, so the import copies it
 * from there once (Storage Access Framework), as other ports of PC games do.
 */
object GameData {
    // Files every PC edition has; the check MissingGameData does in src/platform/sys.cpp.
    private val REQUIRED = listOf("Game/Bigfiles/common.bfl", "FrontEnd/common.bfl", "Sounds/surface.bfl")

    private fun storage(context: Context): File = context.getExternalFilesDir(null) ?: context.filesDir

    /** Where the game data lives; passed to the game with --data. */
    fun directory(context: Context): File = File(storage(context), "game")

    fun isInstalled(context: Context): Boolean {
        val dir = directory(context)
        return REQUIRED.all { path ->
            val file = path.split('/').fold<String, File?>(dir) { parent, name ->
                parent?.listFiles()?.firstOrNull { it.name.equals(name, ignoreCase = true) }
            }
            file != null && file.isFile && file.length() > 0
        }
    }

    sealed interface Progress {
        data object Searching : Progress
        data class Copying(val copied: Long, val total: Long) : Progress
    }

    sealed class ImportException(message: String) : IOException(message) {
        class NotFound : ImportException("No installation found")
        class Ambiguous(val folders: List<String>) : ImportException("More than one installation found")
        class NoSpace(val needed: Long, val available: Long) : ImportException("Not enough space")
    }

    /**
     * Copies the installation in or under [tree] (a folder from
     * ACTION_OPEN_DOCUMENT_TREE) to [directory]. The copy goes to a temporary
     * folder first, so an interrupted import leaves nothing half done.
     */
    suspend fun import(context: Context, tree: Uri, onProgress: (Progress) -> Unit) {
        onProgress(Progress.Searching)
        val source = DocumentTree(context.contentResolver, tree)
        val found = source.findInstallations()
        val root = when (found.size) {
            0 -> throw ImportException.NotFound()
            1 -> found.single()
            else -> throw ImportException.Ambiguous(found.map { it.path })
        }
        val files = source.listFiles(root.documentId)
        val total = files.sumOf { it.second.size }

        val target = directory(context)
        val temporary = File(storage(context), "game.import")
        temporary.deleteRecursively()
        // The space of an earlier copy is freed when this one replaces it.
        val available = StatFs(storage(context).path).availableBytes + target.walkBottomUp().sumOf { it.length() }
        val margin = 64L shl 20
        if (available < total + margin)
            throw ImportException.NoSpace(total + margin, available)

        try {
            var copied = 0L
            var reported = 0L
            val buffer = ByteArray(1 shl 20)
            onProgress(Progress.Copying(0, total))
            for ((path, entry) in files) {
                val file = File(temporary, path)
                file.parentFile!!.mkdirs()
                source.open(entry.documentId).use { input ->
                    file.outputStream().use { output ->
                        while (true) {
                            coroutineContext.ensureActive()
                            val n = input.read(buffer)
                            if (n < 0) break
                            output.write(buffer, 0, n)
                            copied += n
                            if (copied - reported >= (4 shl 20)) {
                                onProgress(Progress.Copying(copied, total))
                                reported = copied
                            }
                        }
                    }
                }
            }
            onProgress(Progress.Copying(total, total))
            target.deleteRecursively()
            if (!temporary.renameTo(target))
                throw IOException("Cannot move the copied files to $target")
        } finally {
            temporary.deleteRecursively()
        }
    }

    private class DocumentTree(private val resolver: ContentResolver, private val tree: Uri) {
        data class Entry(val documentId: String, val name: String, val isDirectory: Boolean, val size: Long)
        data class Folder(val documentId: String, val path: String)

        fun children(documentId: String): List<Entry> {
            val uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, documentId)
            val projection = arrayOf(
                Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE,
            )
            val entries = mutableListOf<Entry>()
            resolver.query(uri, projection, null, null, null)?.use { cursor ->
                while (cursor.moveToNext()) {
                    entries += Entry(
                        documentId = cursor.getString(0),
                        name = cursor.getString(1) ?: continue,
                        isDirectory = cursor.getString(2) == Document.MIME_TYPE_DIR,
                        size = if (cursor.isNull(3)) 0 else cursor.getLong(3),
                    )
                }
            }
            return entries
        }

        fun open(documentId: String) =
            resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, documentId))
                ?: throw IOException("Cannot open $documentId")

        private fun isInstallation(documentId: String): Boolean = REQUIRED.all { path ->
            var entry: Entry? = null
            var parent = documentId
            for (name in path.split('/')) {
                entry = children(parent).firstOrNull { it.name.equals(name, ignoreCase = true) } ?: return@all false
                parent = entry.documentId
            }
            entry != null && !entry.isDirectory && entry.size > 0
        }

        /**
         * The chosen folder when it is an installation, otherwise the ones
         * found up to three levels below it (a download folder around it, as
         * on the desktop). Found installations are not searched further.
         */
        fun findInstallations(): List<Folder> {
            val pending = ArrayDeque(listOf(Folder(DocumentsContract.getTreeDocumentId(tree), "") to 0))
            val found = mutableListOf<Folder>()
            var visited = 0
            while (pending.isNotEmpty() && visited++ < 512) {
                val (folder, depth) = pending.removeFirst()
                if (isInstallation(folder.documentId)) {
                    found += folder
                    continue
                }
                if (depth == 3) continue
                for (child in children(folder.documentId)) {
                    if (child.isDirectory && !child.name.startsWith('.'))
                        pending += Folder(child.documentId, folder.path + "/" + child.name) to depth + 1
                }
            }
            return found
        }

        /** Every file under the folder, with its path relative to it. */
        fun listFiles(documentId: String): List<Pair<String, Entry>> {
            val files = mutableListOf<Pair<String, Entry>>()
            fun walk(id: String, prefix: String) {
                for (child in children(id)) {
                    if (child.isDirectory) walk(child.documentId, prefix + child.name + "/")
                    else files += prefix + child.name to child
                }
            }
            walk(documentId, "")
            return files
        }
    }
}
