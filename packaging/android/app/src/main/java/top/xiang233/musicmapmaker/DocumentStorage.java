package top.xiang233.musicmapmaker;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.ReentrantLock;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * SAF 授权与应用私有工作副本之间的边界，不向 C++ 伪装 content URI 为磁盘路径。
 * 系统选择器在 UI 线程启动，资源复制在串行文件线程完成，SDL 主线程不等待授权。
 * URI、授权与内容基线持久化到本包 files/documents，配置升级后仍可定位原文档。
 * 保存仅发布指定文件，不遍历或删除用户授权树中的其他内容。
 */
final class DocumentStorage {
    /** 独立请求码不会抢占 SDL 已有的 Activity 返回处理。 */
    static final int REQUEST = 7341;
    /** Activity 只用于系统选择器与 ContentResolver，不跨应用申请存储路径。 */
    private final Activity activity;
    /** 工作副本和映射都属于本应用私有目录，不需要共享存储权限。 */
    private final File root;
    /** 映射文件最后原子替换，避免进程中断留下半个 JSON。 */
    private final File index;
    /** 文件操作单线程调度，避免选择导入与保存同时更新映射。 */
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    /** 只发布一次结果，原生侧非阻塞取走；不使用固定时间轮询等待。 */
    private final AtomicReference<String> result = new AtomicReference<>();
    /** 只由 UI 线程读写的在途标记，一个系统选择器对应一个回调。 */
    private boolean pending;
    /** 当前选择类型：0 打开单文件、1 创建保存目标、2 授权工程目录。 */
    private int kind;
    /** 映射由 fileGate 保护，UI 帧不读取或等待文件操作。 */
    private JSONObject mounts;
    /** 文件操作门闩只允许工作线程等待，原生保存使用 tryLock 立即返回忙状态。 */
    private final ReentrantLock fileGate = new ReentrantLock();

    /**
     * @brief 建立目录身份，磁盘读取延迟到实际用户操作的工作线程。
     * @param activity 已 attach 的当前 Activity，不能在其构造阶段访问内部路径。
     * @details 只记录私有目录身份，首次磁盘读取由 load 在文档操作中完成。
     * @warning 对象与当前 Activity 同生命周期，不获取其他应用窗口或目录。
     */
    DocumentStorage(Activity activity) {
        this.activity = activity;
        root = new File(activity.getFilesDir(), "documents");
        index = new File(root, "mounts.json");
    }

    /**
     * @brief 由 Activity 的 UI 线程发起选择，不在此等待系统返回或读取工程文件。
     * @param requestKind 分别表示单文件打开、创建目标和目录树授权。
     * @param name 保存时的建议文件名，不携带目录或未经授权的磁盘位置。
     * @details 最终写权限仍以返回 flags 为准，不能用请求 flags 假定可写。
     */
    void begin(int requestKind, String name) {
        if (pending) { result.set("ERROR\n另一个文档选择尚未结束"); return; }
        pending = true;
        kind = requestKind;
        Intent intent = new Intent(requestKind == 2 ? Intent.ACTION_OPEN_DOCUMENT_TREE
                : requestKind == 1 ? Intent.ACTION_CREATE_DOCUMENT : Intent.ACTION_OPEN_DOCUMENT);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        if (requestKind != 2) {
            // MMM 等自有扩展名未必有系统 MIME 类型，不能被错误过滤为不可选。
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            if (requestKind == 1) intent.putExtra(Intent.EXTRA_TITLE, name);
        }
        try { activity.startActivityForResult(intent, REQUEST); }
        catch (RuntimeException error) { complete("ERROR\n" + error.getMessage()); }
    }

    /**
     * @brief 只处理本类请求码，取消与权限错误分别返回原生端。
     * @param requestCode 系统返回的请求码，不属于本服务的交回 SDL。
     * @return 当前返回是否已消费；取消不创建工作副本或改变活动工程。
     * @warning 资源复制调度到文件线程，Java UI 线程不得等待 provider 网络读取。
     */
    boolean accept(int requestCode, int status, Intent data) {
        if (requestCode != REQUEST) return false;
        if (status != Activity.RESULT_OK || data == null || data.getData() == null) {
            complete("CANCEL\n");
            return true;
        }
        Uri uri = data.getData();
        int flags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        int selectedKind = kind;
        // 系统授权先持久化，随后才复制资源，避免长工程导入占住 Java UI 线程。
        try { activity.getContentResolver().takePersistableUriPermission(uri, flags); }
        catch (RuntimeException error) { complete("ERROR\n无法持久化文档授权：" + error.getMessage()); return true; }
        worker.execute(() -> {
            try { complete("OK\n" + materialize(uri, selectedKind, flags)); }
            catch (Exception error) { complete("ERROR\n" + error.getMessage()); }
        });
        return true;
    }

    /**
     * @brief 原生端只取已经完成的短结果，不复制正在导入的工程数据。
     * @return 标准 UTF-8 的状态与路径；未完成时为 null。
     * @details 原子取走结果避免两个帧重复执行同一次工程打开回调。
     * @warning JNI 调用线程只消费值对象，不访问 worker 正在维护的映射。
     */
    byte[] takeResult() {
        String value = result.getAndSet(null);
        return value == null ? null : value.getBytes(StandardCharsets.UTF_8);
    }

    /**
     * @brief 发布结果后解除 UI 在途状态，下一次选择不会抢占旧结果。
     * @param value 成功、取消或错误的完整载荷。
     * @details Java UI 线程独占 pending，文件线程仅通过投递事件清除此标志。
     * @warning 原生业务回调必须由 SDL UI 帧消费，不从 Java 文件线程直接调用。
     */
    private void complete(String value) {
        result.set(value);
        activity.runOnUiThread(() -> pending = false);
    }

    /**
     * @brief 首次文件操作加载持久索引；损坏时拒绝覆盖授权记录。
     * @details 索引保存本包授权 URI、相对布局与导入内容的摘要。
     * @throws Exception 索引损坏或私有目录不可写时向平台边界报告失败。
     * @warning 只能在文件门闩保护的低频操作中调用，禁止逐帧重读。
     */
    private void load() throws Exception {
        if (mounts != null) return;
        Files.createDirectories(root.toPath());
        mounts = index.isFile() ? new JSONObject(new String(Files.readAllBytes(index.toPath()), StandardCharsets.UTF_8)) : new JSONObject();
    }

    /**
     * @brief 索引是本应用私有元数据，临时文件与替换目标位于相同文件系统。
     * @details 先写临时文件再原子替换，进程中断不会发布半个 JSON。
     * @throws Exception 更新失败时保留工作副本，不伪造已经同步的基线。
     * @warning 映射不是工程资源，不能复制到用户授权目录或源码仓库。
     */
    private void persist() throws Exception {
        File temporary = new File(root, "mounts.json.tmp");
        Files.write(temporary.toPath(), mounts.toString().getBytes(StandardCharsets.UTF_8));
        Files.move(temporary.toPath(), index.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
    }

    /**
     * @brief URI 的摘要只用于稳定目录身份，不能拿它代替文件内容冲突基线。
     * @param bytes URI 的标准 UTF-8 表示，与展示名称无关。
     * @return 固定长度 SHA-256 文本，避免目录名携带 URI 的保留字符。
     * @details 同名不同 URI 不会合并到同一工作副本。
     */
    private static String digest(byte[] bytes) throws Exception {
        byte[] hash = MessageDigest.getInstance("SHA-256").digest(bytes);
        StringBuilder text = new StringBuilder();
        for (byte value : hash) text.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return text.toString();
    }

    /**
     * @brief 文件名必须是单个路径分量，拒绝 provider 给出的越界名称。
     * @param name provider 返回的展示名，必须按不可信输入检查。
     * @return 完整原名；不通过静默改名掩盖资源冲突。
     * @throws IOException 父目录、路径分隔符或空名称不能进入本地复制。
     */
    private static String safeName(String name) throws IOException {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")
                || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf('\0') >= 0)
            throw new IOException("文档提供者返回了无效文件名");
        return name;
    }

    /**
     * @brief 标准列适用于本机与云端 provider，不查询不受支持的 _data 真实路径。
     * @param uri 已通过系统选择的授权文档。
     * @return 校验后的单个名称，不返回或推断共享磁盘路径。
     * @warning Cursor 生命周期限制在本次查询，不能借用到后续异步操作。
     */
    private String nameOf(Uri uri) throws Exception {
        try (Cursor cursor = activity.getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor == null || !cursor.moveToFirst()) throw new IOException("无法读取文档名称");
            return safeName(cursor.getString(0));
        }
    }

    /**
     * @brief 复制与 SHA 同时流式计算，音频资源不需要整份加载到 Java 内存。
     * @param destination 私有工作根之内的已校验目标。
     * @return 导入字节的摘要，后续写回先与外部内容进行冲突比较。
     * @details 两个流都在失败或成功时关闭，不发布部分内容的成功映射。
     */
    private String copy(Uri uri, File destination) throws Exception {
        Files.createDirectories(destination.getParentFile().toPath());
        MessageDigest hash = MessageDigest.getInstance("SHA-256");
        try (InputStream input = activity.getContentResolver().openInputStream(uri);
             OutputStream output = new FileOutputStream(destination)) {
            if (input == null) throw new IOException("无法读取所选文档");
            byte[] buffer = new byte[65536];
            int count;
            while ((count = input.read(buffer)) != -1) { output.write(buffer, 0, count); hash.update(buffer, 0, count); }
        }
        return hex(hash.digest());
    }

    /**
     * @brief 将摘要字节转成固定长度文本，保存和首次导入共用相同比较表示。
     * @param bytes 完整摘要，不接受文本路径代替内容校验。
     * @return 每字节两位的小写十六进制文本。
     * @details Locale 固定，摘要表示不能随手机语言变化。
     */
    private static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder();
        for (byte value : bytes) text.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return text.toString();
    }

    /**
     * @brief 只计算正在保存的一个文档，不每帧扫描工程或重新哈希大音频。
     * @param uri 当前已授权的外部保存目标。
     * @return 从 provider 实际读出的内容摘要。
     * @throws Exception 授权撤回、网络或读盘错误必须阻止覆盖成功状态。
     */
    private String hash(Uri uri) throws Exception {
        MessageDigest hash = MessageDigest.getInstance("SHA-256");
        try (InputStream input = activity.getContentResolver().openInputStream(uri)) {
            if (input == null) throw new IOException("无法校验目标文档");
            byte[] buffer = new byte[65536]; int count;
            while ((count = input.read(buffer)) != -1) hash.update(buffer, 0, count);
        }
        return hex(hash.digest());
    }

    /**
     * @brief 授权目录按 provider ID 遍历，路径层级完全保留给现有 C++ 相对资源引用。
     * @param visited 检测重复和循环 ID，不能无限跟随异常 provider 目录结构。
     * @param files 只有复制完成的普通文件才能登记 URI 与摘要。
     * @warning 同级重名条目必须拒绝，不能依据枚举顺序覆盖工程资源。
     */
    private void copyTree(Uri tree, String id, File directory, String relative,
                          JSONObject files, Set<String> visited) throws Exception {
        if (!visited.add(id)) throw new IOException("文档目录包含重复或循环 ID");
        Files.createDirectories(directory.toPath());
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor cursor = activity.getContentResolver().query(children, columns, null, null, null)) {
            if (cursor == null) throw new IOException("无法列出所选工程目录");
            Set<String> names = new HashSet<>();
            while (cursor.moveToNext()) {
                String childId = cursor.getString(0), name = safeName(cursor.getString(1));
                if (!names.add(name)) throw new IOException("同一目录存在重名文档：" + name);
                File child = new File(directory, name);
                String path = relative.isEmpty() ? name : relative + "/" + name;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2))) {
                    copyTree(tree, childId, child, path, files, visited);
                } else {
                    Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, childId);
                    files.put(path, new JSONObject().put("uri", document.toString()).put("hash", copy(document, child)));
                }
            }
        }
    }

    /**
     * @brief 生成完整工作副本后才发布映射，失败不会把不完整工程交给 C++。
     * @return 目录或单文件的普通私有路径，URI 始终留在本服务。
     * @details 已存在副本可能含未发布修改，重复选择不自动覆盖其内容。
     * @warning 只有文件线程允许等待门闩，原生保存调用使用立即失败的 tryLock。
     */
    private String materialize(Uri uri, int requestKind, int flags) throws Exception {
        fileGate.lock();
        try {
        load();
        String key = digest(uri.toString().getBytes(StandardCharsets.UTF_8));
        File directory = new File(root, key);
        // 已有副本可能含尚未发布的修改，重复选择不能静默用外部字节覆盖它。
        if (mounts.has(key) && directory.isDirectory()) return mounts.getJSONObject(key).getString("path");
        JSONObject files = new JSONObject();
        JSONObject mount = new JSONObject().put("source", uri.toString()).put("tree", requestKind == 2)
                .put("writable", (flags & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0).put("files", files);
        Files.createDirectories(directory.toPath());
        String path;
        if (requestKind == 2) {
            copyTree(uri, DocumentsContract.getTreeDocumentId(uri), directory, "", files, new HashSet<>());
            path = directory.getCanonicalPath();
        } else {
            String name = nameOf(uri);
            File target = new File(directory, name);
            // CREATE_DOCUMENT 已创建外部占位文件，但本地先不创建，保留应用的覆盖确认语义。
            String baseline = requestKind == 1 ? hash(uri) : copy(uri, target);
            files.put(name, new JSONObject().put("uri", uri.toString()).put("hash", baseline));
            path = target.getCanonicalPath();
        }
        mount.put("path", path);
        mounts.put(key, mount);
        persist();
        return path;
        } finally { fileGate.unlock(); }
    }

    /**
     * @brief 沿授权树寻找或创建目标父目录，不根据 URI 文本拼接假想磁盘路径。
     * @param relative 工作根中的相对文件名，所有分量均重新验证。
     * @return 已授权树内的目标文档 URI。
     * @throws Exception 外部出现未登记同名文件或目录类型冲突时拒绝覆盖。
     */
    private Uri ensureTarget(JSONObject mount, String relative) throws Exception {
        Uri tree = Uri.parse(mount.getString("source"));
        Uri parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        String[] parts = relative.split("/");
        for (int i = 0; i < parts.length; ++i) {
            String name = safeName(parts[i]);
            boolean directory = i + 1 < parts.length;
            Uri found = null;
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getDocumentId(parent));
            try (Cursor cursor = activity.getContentResolver().query(children,
                    new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                            DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
                if (cursor == null) throw new IOException("无法查询保存目标目录");
                while (cursor.moveToNext()) if (name.equals(cursor.getString(1))) {
                    if (directory != DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2)))
                        throw new IOException("保存目标的目录类型冲突：" + name);
                    // 未登记的新目标若已存在，不能跳过内容基线直接覆盖其他应用创建的文档。
                    if (!directory) throw new IOException("外部出现同名保存目标，请重新选择：" + name);
                    found = DocumentsContract.buildDocumentUriUsingTree(tree, cursor.getString(0)); break;
                }
            }
            if (found == null) found = DocumentsContract.createDocument(activity.getContentResolver(), parent,
                    directory ? DocumentsContract.Document.MIME_TYPE_DIR : "application/octet-stream", name);
            if (found == null) throw new IOException("无法创建保存目标：" + name);
            parent = found;
        }
        return parent;
    }

    /**
     * @brief 写回指定文档；外部内容已变化时拒绝覆盖，并保留本地修改供用户重试或另存。
     * @param filename writer 已关闭的普通工作副本路径，不是 content URI。
     * @return null 表示平台发布成功，其他诊断必须作为保存失败返回。
     * @warning 只核对指定文件，输出流关闭并读回摘要后才推进持久冲突基线。
     */
    String publish(String filename) {
        // 外部导入可能很长，保存调用不得等待另一工程完整复制。
        if (!fileGate.tryLock()) return "另一个文档操作进行中，本地修改已保留，请重试保存";
        try {
            load();
            File source = new File(filename).getCanonicalFile();
            // /data/user/0 与 /data/data 可能互为别名，两端必须先统一为规范路径。
            File canonicalRoot = root.getCanonicalFile();
            if (!source.toPath().startsWith(canonicalRoot.toPath())) return null;
            String subpath = canonicalRoot.toPath().relativize(source.toPath()).toString();
            int separator = subpath.indexOf('/');
            if (separator < 0) return null;
            String key = subpath.substring(0, separator), relative = subpath.substring(separator + 1);
            if (!mounts.has(key)) throw new IOException("工作副本缺少 SAF 授权映射");
            JSONObject mount = mounts.getJSONObject(key), files = mount.getJSONObject("files");
            if (!mount.getBoolean("writable")) throw new IOException("所选文档没有写入授权");
            JSONObject entry = files.optJSONObject(relative);
            Uri target;
            if (entry != null) {
                target = Uri.parse(entry.getString("uri"));
                // 每次保存先核对当前外部内容，不能覆盖手机其他编辑器的新修改。
                if (!hash(target).equals(entry.getString("hash"))) throw new IOException("外部文档已被修改，请另存或重新导入");
            } else {
                if (!mount.getBoolean("tree")) throw new IOException("单文件授权不能创建其他文档");
                target = ensureTarget(mount, relative);
                entry = new JSONObject().put("uri", target.toString());
            }
            MessageDigest hash = MessageDigest.getInstance("SHA-256");
            try (InputStream input = new FileInputStream(source);
                 OutputStream output = activity.getContentResolver().openOutputStream(target, "rwt")) {
                if (output == null) throw new IOException("无法写入授权文档");
                byte[] buffer = new byte[65536]; int count;
                while ((count = input.read(buffer)) != -1) { output.write(buffer, 0, count); hash.update(buffer, 0, count); }
            }
            String expected = hex(hash.digest());
            if (!expected.equals(hash(target))) throw new IOException("文档提供者写入后内容校验失败");
            // 外部写入和读回校验完成后才推进冲突基线，失败不会被报告为保存成功。
            entry.put("hash", expected); files.put(relative, entry); persist();
            return null;
        } catch (Exception error) { return "SAF 保存失败，本地副本已保留：" + error.getMessage(); }
        finally { fileGate.unlock(); }
    }
}
