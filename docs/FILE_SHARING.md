# Files between iOS and Linux

Open **Settings → Files & Devices → File Sharing** for the selected environment. The File Sharing screen groups Import Files, Export Files and Shared Folders. **Choose Destination & Import**, **Select File or Folder to Export** and **Open Shared Folder** lead to the same guarded **Shared Files** browser. **Add or Manage Shared Folders** retains the native Files picker and existing security-scoped folder bookmarks. The browser works with that environment’s folder, independently of the current terminal directory.

## Import

1. Open the destination folder in Shared Files; **New Folder** creates a subfolder.
2. Choose **Import File Here**, then select a regular file in the native Files picker.
3. Wait for completion. In Linux, open `/mnt/LinuxRunnerShared` or `~/Shared`.

The configured Linux account receives a `~/Shared` alias when that name is free. An existing file, folder or alias is preserved. `/mnt/LinuxRunnerShared` remains the explicit shared location. This is one folder per environment, not a separate private folder for each Linux account.

In iOS Files, browse **On My iPhone/iPad → LinuxRunner → Shared → <environment root name>**. Existing Downloads exchanges and external-folder bookmarks are retained; the browser links to **Previous Downloads Exchange**. No automatic migration deletes or replaces those files.

## Export

Select a file, then **Save to Files…** and choose a destination in the native picker. For a directory, open it and choose **Export This Folder as ZIP**, or use its context menu. Export reads a temporary snapshot; the Linux source is retained. Finish editing files before exporting them.

## Limits and recovery

- Data streams in 64 KiB blocks; imports and exports are limited to 512 MiB. ZIP exports additionally allow at most 4096 entries and 32 directory levels, with no compression.
- Unicode filenames are preserved. Existing names are never overwritten; choose another destination or rename the source.
- Symbolic links, special files and paths escaping the shared directory are refused. Some Linux filenames containing backslashes cannot be transferred through this browser.
- Cancellation removes the unpublished temporary copy. A transfer interrupted by app termination leaves no partially published destination; private import staging is cleaned at the next initialization. Retry the original source after reopening.
- A source changed during copying causes an error rather than a verified-success claim. This is change detection, not a filesystem-wide transactional snapshot.
- Cloud providers may need connectivity or time to download the chosen source. iOS permissions still apply.

Files-provider behavior depends on the platform and provider. Test access with a copy of important data, and report reproducible failures through the [troubleshooting guide](TROUBLESHOOTING.md).
