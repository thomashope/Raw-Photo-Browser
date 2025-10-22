# Photo Browser

## Planned Features

- UI for browsing and selecting files with thumbnails, displaying raw files, and viewing meta data
- Searching and filtering of files
- Don't consume heaps of battery all the time (only render and flip the buffer when necessary)
- Fast image loading (use threads and load images in the background)
- Cross platform (mac and windows)

## Planned Optimisations

- cache the rawProcessor in the database (when not in use by a worker thread) so that it can be used by subsequent load operations
  - (if both preview and raw images are loaded, the rawProcessor can always be discarded)
- Tasks may need to have an ID to optimize task scheduling. e.g. so PreviewOnly and RawOnly tasks can be merged

## TODO Next

...

## Other Tasks

# Claude Code Observations

- Eventually you can exhaust the context and the chat gets converted into a summary and a new thread is launched (not sure if this is a feature of claude code or of Zed?).
- Assuming the context window is a limited resource, it may be better to do simple changes by hand. E.g. fixing trivial bugs or renaming things. The downside of this is that Claude then has to reread files again, likely consuming more context
- Had to keep an eye out for duplicate code. Either because claude 'forgot' how an API worked, or because it would not refactor out common code unless asked.
