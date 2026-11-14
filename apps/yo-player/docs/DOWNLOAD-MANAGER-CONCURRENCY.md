# Download manager: startup and concurrency rules

The download manager must never make the application wait for storage during startup
or while drawing a frame. These rules are part of the implementation contract.

## Startup path

`initDownloads()` is called during application initialization. It may only:

- clear in-memory state;
- create the small queue/cache mutexes;
- initialize the overlay labels.

It must **not** call `makeDir`, `openDir`, `readDir`, `statPath`, `fileExists`,
`readFile`, `writeFile`, `measureTree`, `getFreeSpace`, `copyFile`, or any other
filesystem operation. It must not scan the download library, migrate staged MP4s,
or wait for a worker. A slow, unmounted, or large HDD must not prevent the first
frame from being displayed.

## Thread ownership

- The PPU/UI thread owns controller input, label changes, and rendering.
- The download worker owns network downloads, MP4 remuxing, and download writes.
- The library worker is created lazily when the user enters Downloads or explicitly
  opens the manager. It owns directory enumeration, metadata reads, legacy staging
  migration, library deletion, recursive size measurement, and free-space queries.
- The library worker runs at `THREAD_PRIORITY_LOW` and sleeps while idle. It does not
  exist at startup unless the user has requested library work. Scans may overlap
  downloads because active/queued IDs are skipped and MP4 publication is by rename;
  deletion also skips IDs that are active or queued and reserves each ID against a
  concurrent re-enqueue until its sidecars and MP4 are removed.

## Cache and locks

The Completed and Failed tabs read only the manager's in-memory cache. The Ongoing
tab copies the active item and queue under the download mutex. Getters may copy
bounded arrays while holding a lock, but must never perform filesystem or network
I/O while a lock is held.

The only nested lock order is `dl.mutex` then `manager.mutex`, used by enqueue to
check a deletion reservation. The library worker must always release
`manager.mutex` before checking the download queue; never acquire `dl.mutex` while
holding `manager.mutex`. All other paths use one lock at a time. Worker I/O is
performed after releasing both locks. Publishing a scan is a short memory-copy
critical section.

## Rendering and update callbacks

`drawDownloadManagement()`, `updateDownloadManagement()`, and
`updateDownloadSpaceStats()` must remain filesystem-free. They may:

- read cached data through the bounded getters;
- update labels and selection state;
- enqueue a refresh or deletion request.

They must not enumerate directories, read metadata, inspect MP4 atoms, measure a
directory tree, query free space, copy files, or delete files. The scroll view only
draws the seven visible rows; it does not allocate a full library snapshot per frame.

## Migration and file visibility

Legacy staged MP4 migration is deferred to the library worker. MP4s are copied to a
temporary `.migrate` path and renamed to their final `.mp4` name only after the copy
succeeds. The grid and library scanner therefore never treat a partially copied MP4
as a completed download.

## Download state classification

- Ongoing is a live snapshot of the active download and queue.
- Completed contains valid `.mp4` files, with `.meta` used only for the display title.
- Failed contains `.failed` markers, legacy `.meta` sidecars without a matching
  `.mp4`, or zero-byte `.mp4` placeholders. The download worker writes `.failed` on
  a genuine failure and removes it after a successful completion.
- User-cancelled downloads and downloads declined at the disk-space confirmation are
  not marked Failed; user cancellation cleans up partial artifacts.

## Shutdown

Shutdown first signals both workers to stop, then joins only workers that were
actually created, and only then destroys their mutexes and UI resources. Do not free
cache state or destroy a lock while a worker can still access it.

When changing this code, test startup with a large download directory, opening and
closing Downloads during a scan, cancelling active and queued downloads, deleting
multiple Completed/Failed items, and exiting while each worker is active.
