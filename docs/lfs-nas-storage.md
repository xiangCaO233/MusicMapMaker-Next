# NAS Git LFS 对象存储

主仓与 `IonCachyEngine` 的 Git 提交、分支、标签和代码继续保存在 GitHub；两仓的全部 LFS 对象共用以下公共对象库：

```text
https://gitea.xiang233.top/xiang/mmm-lfs.git/info/lfs
```

对象内容包括预编译库及配套头文件、字体、图片、测试音频、谱包等原本由 LFS 跟踪的资源。`.gitattributes`、LFS 指针及其 SHA256 身份不改变，不重写历史或移动发布标签。NAS 对象库按内容去重，不需要为两仓重复保存相同对象。

## 拉取与上传

当前分支的 `.lfsconfig` 提供服务地址。主仓保留原有 `fetchexclude = *`，构建脚本仍按平台和依赖来源显式选择对象；迁移不会让每次构建自动下载全部历史资源。引擎仅增加服务地址，不改变原有下载策略。

NAS 对象库允许匿名下载。普通构建、CI 和新开发者拉取无需 GitHub LFS 配额，也不需要 NAS 上传令牌。对象缺失、NAS 不可达或权限错误应明确失败，不能自动回退到 GitHub 并重新产生托管费用。

上传需要开发者自己的 NAS Gitea 仓库写权限。令牌仅放在凭据管理器或受保护的本机凭据文件中，不得写入 `.lfsconfig`、remote URL、脚本、日志或提交。源码的 `origin` 仍指向各自 GitHub 仓库，Git 与 LFS 服务地址彼此独立。

## 检出旧分支或旧标签

旧提交可能没有 NAS 的 `.lfsconfig`。为保持不可变标签不变，应在本机仓库的 Git 元数据中显式指定 LFS 地址：

```bash
git config --local lfs.url https://gitea.xiang233.top/xiang/mmm-lfs.git/info/lfs
git -C 3rdpty/sources/IonCachyEngine config --local lfs.url https://gitea.xiang233.top/xiang/mmm-lfs.git/info/lfs
```

首次取得尚未迁移配置的旧源码时，先跳过自动 smudge，再指定地址并按需拉取：

```bash
GIT_LFS_SKIP_SMUDGE=1 git clone <GitHub 仓库地址> <工作目录>
git -C <工作目录> config --local lfs.url https://gitea.xiang233.top/xiang/mmm-lfs.git/info/lfs
git -C <工作目录> lfs pull --include='<需要的资源或平台路径>' --exclude=''
```

引擎子模块使用同一对象库；初始化子模块后也应设置其本地地址，再拉取需要的依赖。主仓现有 `scripts/ci/pull-lfs-for-build.sh` 继续管理按平台拉取，`SOURCES_BUILD=OFF` 缺包时仍须失败。

## 迁移验收与 GitHub 清理

迁移对象按两仓全部保留 refs 的 LFS 清单汇总，并纳入主仓历史 gitlink 指向的引擎提交。服务器上传与校验必须完成，再检查全量对象存在性，并在不使用上传凭据的独立缓存中下载代表性对象、校验大小与 SHA256。两个 `.lfsconfig` 应在源码中发布，既有 Runner 和开发机也需要保留旧标签的地址覆盖。

NAS 上传成功不等于 GitHub 已释放存储。GitHub 官方说明，已删除或不再使用的 LFS 对象仍可能计费；保留仓库时应联系 GitHub Support 清除旧 LFS 对象。清理范围仅限以下两仓的 GitHub LFS 对象：

- `xiangCaO233/MusicMapMaker-Next`
- `xiangCaO233/IonCachyEngine`

Support 请求应明确保留 Git 仓库、提交、分支、标签、Release、PR、issue、star 和 fork。不得为释放 LFS 配额而删除重建仓库，也不能把本地 `git lfs prune` 当作 GitHub 远端清理完成。需取得支持处理结果及后续存储用量确认，才能报告 GitHub 清理完成。

参考：[Git LFS 服务发现](https://github.com/git-lfs/git-lfs/blob/main/docs/api/server-discovery.md)、[GitHub LFS 对象清理](https://docs.github.com/en/repositories/working-with-files/managing-large-files/removing-files-from-git-large-file-storage)。
