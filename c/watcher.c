#include "watcher.h"

#include <CoreServices/CoreServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fts.h>
#include <stdio.h>
#include <string.h>
#include "util.h"
#include "protocol.h"

const size_t RENAMED_EVENTS_COUNT = 2;
const size_t FILE_MODIFIED_VIA_TMP_EVENTS_COUNT = 2;

static inline bool has_flag(FSEventStreamEventFlags flag, FSEventStreamEventFlags bit)
{
    return (flag & bit) != 0;
}

bool is_file_modified_via_tmp_file(size_t events_count, const FSEventStreamEventFlags *eventFlags, const char *const *paths)
{
    if (events_count < FILE_MODIFIED_VIA_TMP_EVENTS_COUNT)
    {
        return false;
    }

    if (strcmp(paths[0], paths[1]) != 0)
    {
        return false;
    }

    if (has_flag(eventFlags[0], kFSEventStreamEventFlagItemRemoved) && has_flag(eventFlags[0], kFSEventStreamEventFlagItemIsFile) && has_flag(eventFlags[1], kFSEventStreamEventFlagItemIsFile) &&
        has_flag(eventFlags[1], kFSEventStreamEventFlagItemRenamed))
    {
        return true;
    }
    return false;
}

typedef bool (*send_object_fn)(ObjectType object_type, const char *path);

static void send_folder_contents_recursive(const char *folder_path, send_object_fn sender)
{
    if (!folder_path || !sender)
    {
        return;
    }

    char *paths[] = {(char *)folder_path, NULL};
    FTS *tree = fts_open(paths, FTS_NOCHDIR | FTS_PHYSICAL, NULL);
    if (tree == NULL)
    {
        fprintf(stderr, "failed to open folder tree: %s\n", folder_path);
        return;
    }

    FTSENT *node = NULL;
    while ((node = fts_read(tree)) != NULL)
    {
        if (node->fts_level == 0)
        {
            continue;
        }

        if (is_DS_Store_path(node->fts_path))
        {
            continue;
        }

        if (node->fts_info == FTS_F)
        {
            sender(OBJECT_FILE, node->fts_path);
        }
        else if (node->fts_info == FTS_D)
        {
            sender(OBJECT_FOLDER, node->fts_path);
        }
    }

    fts_close(tree);
}

bool is_object_renamed(FSEventStreamEventFlags object_flag, size_t events_count, const FSEventStreamEventFlags *eventFlags, const FSEventStreamEventId *eventIds, const char *const *paths)
{
    if (events_count < RENAMED_EVENTS_COUNT)
    {
        return false;
    }

    // first events id is x, seconds is x+1
    if (eventIds[0] + 1 != eventIds[1])
    {
        return false;
    }

    for (size_t i = 0; i < RENAMED_EVENTS_COUNT; i++)
    {
        bool is_it_the_same_object_type = has_flag(eventFlags[i], object_flag);
        bool is_renamed = has_flag(eventFlags[i], kFSEventStreamEventFlagItemRenamed);

        if (!(is_it_the_same_object_type && is_renamed))
        {
            return false;
        }
    }

    // the second path is the new name
    if (!does_object_exist(paths[1]))
    {
        return false;
    }

    return true;
}

bool is_file_renamed(size_t events_count, const FSEventStreamEventFlags *eventFlags, const FSEventStreamEventId *eventIds, const char *const *paths)
{
    return is_object_renamed(kFSEventStreamEventFlagItemIsFile, events_count, eventFlags, eventIds, paths);
}

bool is_folder_renamed(size_t events_count, const FSEventStreamEventFlags *eventFlags, const FSEventStreamEventId *eventIds, const char *const *paths)
{
    return is_object_renamed(kFSEventStreamEventFlagItemIsDir, events_count, eventFlags, eventIds, paths);
}

static bool is_object_removed(FSEventStreamEventFlags object_flag, FSEventStreamEventFlags flag, const char *file_path)
{
    bool is_renamed = has_flag(flag, kFSEventStreamEventFlagItemRenamed);
    bool is_deleted = has_flag(flag, kFSEventStreamEventFlagItemRemoved);

    if (!has_flag(flag, object_flag))
    {
        return false;
    }

    if (is_renamed && !does_object_exist(file_path))
    {
        return true;
    }

    if (is_deleted)
    {
        return true;
    }

    return false;
}

static bool is_file_removed(FSEventStreamEventFlags flag, const char *file_path)
{
    return is_object_removed(kFSEventStreamEventFlagItemIsFile, flag, file_path);
}

static bool is_folder_removed(FSEventStreamEventFlags flag, const char *file_path)
{
    return is_object_removed(kFSEventStreamEventFlagItemIsDir, flag, file_path);
}

static bool is_object_created(FSEventStreamEventFlags object_flag, FSEventStreamEventFlags flag, const char *file_path)
{
    if (!has_flag(flag, object_flag))
    {
        return false;
    }

    if (has_flag(flag, kFSEventStreamEventFlagItemCreated) && does_object_exist(file_path))
    {
        return true;
    }

    return false;
}

static bool is_file_created(FSEventStreamEventFlags flag, const char *file_path)
{
    return is_object_created(kFSEventStreamEventFlagItemIsFile, flag, file_path);
}

static bool is_folder_created(FSEventStreamEventFlags flag, const char *file_path)
{
    return is_object_created(kFSEventStreamEventFlagItemIsDir, flag, file_path);
}

static bool is_object_added(FSEventStreamEventFlags object_flag, FSEventStreamEventFlags flag, const char *file_path)
{
    if (!has_flag(flag, object_flag))
    {
        return false;
    }

    if (!has_flag(flag, kFSEventStreamEventFlagItemModified) && has_flag(flag, kFSEventStreamEventFlagItemRenamed) && does_object_exist(file_path))
    {
        return true;
    }

    return false;
}

static bool is_file_added(FSEventStreamEventFlags flag, const char *file_path)
{
    return is_object_added(kFSEventStreamEventFlagItemIsFile, flag, file_path);
}

static bool is_folder_added(FSEventStreamEventFlags flag, const char *file_path)
{
    return is_object_added(kFSEventStreamEventFlagItemIsDir, flag, file_path);
}

static bool is_file_modified(FSEventStreamEventFlags flag, const char *file_path)
{
    if (has_flag(flag, kFSEventStreamEventFlagItemIsFile) && has_flag(flag, kFSEventStreamEventFlagItemModified) && does_object_exist(file_path))
    {
        return true;
    }

    return false;
}

static size_t handle_renamed_object(size_t numEvents, const FSEventStreamEventFlags *eventFlags, const FSEventStreamEventId *eventIds, const char *const *paths) {
    size_t consumedEvents = 0;

    if ( is_file_renamed(numEvents, eventFlags, eventIds, paths)) {
        consumedEvents = RENAMED_EVENTS_COUNT;
        send_object_renamed(OBJECT_FILE, paths[0], paths[1]);
    }else if ( is_folder_renamed(numEvents, eventFlags, eventIds, paths) ) {
        consumedEvents = RENAMED_EVENTS_COUNT;
        send_object_renamed(OBJECT_FOLDER, paths[0], paths[1]);
    }

    return consumedEvents;
}

static size_t handle_removed_object(const FSEventStreamEventFlags *eventFlags, const char *const *paths) {
    size_t consumedEvents = 0;

    if (is_file_removed(eventFlags[0], paths[0])) {
        consumedEvents = 1;
        send_object_removed(OBJECT_FILE, paths[0]);
    }else if (is_folder_removed(eventFlags[0], paths[0]) ) {
        consumedEvents = 1;
        send_object_removed(OBJECT_FOLDER, paths[0]);
    }

    return consumedEvents;
}

static size_t handle_created_object(const FSEventStreamEventFlags *eventFlags, const char *const *paths) {
    size_t consumedEvents = 0;

    if ( is_file_created(eventFlags[0], paths[0]) ) {
        consumedEvents = 1;
        send_object_created(OBJECT_FILE, paths[0]);
    }else if ( is_folder_created(eventFlags[0], paths[0]) ) {
        consumedEvents = 1;
        send_object_created(OBJECT_FOLDER, paths[0]);
        send_folder_contents_recursive(paths[0], send_object_created);
    }

    return consumedEvents;
}

static size_t handle_added_object(const FSEventStreamEventFlags *eventFlags, const char *const *paths) {
    size_t consumedEvents = 0;

    if ( is_file_added(eventFlags[0], paths[0]) ) {
        consumedEvents = 1;
        send_object_added(OBJECT_FILE, paths[0]);
    }else if ( is_folder_added(eventFlags[0], paths[0]) ) {
        consumedEvents = 1;
        send_object_added(OBJECT_FOLDER, paths[0]);
        send_folder_contents_recursive(paths[0], send_object_added);
    }

    return consumedEvents;
}

static size_t handle_modified_file(size_t numEvents, const FSEventStreamEventFlags *eventFlags, const char *const *paths) {
    size_t consumedEvents = 0;

    if ( is_file_modified_via_tmp_file(numEvents, eventFlags, paths) ) {
        consumedEvents = FILE_MODIFIED_VIA_TMP_EVENTS_COUNT;
        send_object_modified(OBJECT_FILE, paths[0]);
    }else if ( is_file_modified(eventFlags[0], paths[0]) ) {
        consumedEvents = 1;
        send_object_modified(OBJECT_FILE, paths[0]);
    }

    return consumedEvents;
}

static void stream_callback(ConstFSEventStreamRef streamRef, void *clientCallBackInfo, size_t numEvents, void *eventPaths, const FSEventStreamEventFlags *eventFlags, const FSEventStreamEventId *eventIds)
{
    (void)streamRef;
    (void)clientCallBackInfo;
    const char *const *paths = (const char *const *)eventPaths;

    for (size_t i = 0; i < numEvents;) {
        size_t consumedEvents = 0;

        // skip processing if it is DS_Store
        if (is_DS_Store_path(paths[i]))
        {
            consumedEvents = 1;
        }

        if ( consumedEvents == 0 ) { // handle renamed file/folder
            consumedEvents = handle_renamed_object(numEvents - i, &eventFlags[i], &eventIds[i], &paths[i]);
        }
        if ( consumedEvents == 0 ) { // handle modified file
            consumedEvents = handle_modified_file(numEvents - i, &eventFlags[i], &paths[i]);
        }
        if ( consumedEvents == 0 ) { // handle removed file/folder
            consumedEvents = handle_removed_object(&eventFlags[i], &paths[i]);
        }
        if ( consumedEvents == 0 ) { // handle created file/folder
            consumedEvents = handle_created_object(&eventFlags[i], &paths[i]);
        }
        if ( consumedEvents == 0 ) { // handle added file/folder
            consumedEvents = handle_added_object(&eventFlags[i], &paths[i]);
        }

        if (consumedEvents == 0) {
            consumedEvents = 1;
        }

        i += consumedEvents;
    }

}

bool run_watcher(const char *dir_path, double latency)
{
    if (!dir_path || !does_object_exist(dir_path))
    {
        fprintf(stderr, "invalid path: %s\n", dir_path);
        return false;
    }

    CFStringRef path = CFStringCreateWithCString(NULL, dir_path, kCFStringEncodingUTF8);
    if (!path)
    {
        fprintf(stderr, "failed to create CFString for path\n");
        return false;
    }
    CFArrayRef paths = CFArrayCreate(NULL, (const void *[]){path}, 1, &kCFTypeArrayCallBacks);
    if (paths == NULL)
    {
        fprintf(stderr, "failed to create CFArray for paths\n");
        CFRelease(path);
        return false;
    }
    FSEventStreamCreateFlags flags = kFSEventStreamCreateFlagFileEvents;
    FSEventStreamRef streamRef = FSEventStreamCreate(NULL, stream_callback, NULL, paths, kFSEventStreamEventIdSinceNow, latency, flags);

    if (streamRef == NULL)
    {
        fprintf(stderr, "failed to create stream for filesystem events");
        CFRelease(paths);
        CFRelease(path);
        return false;
    }

    dispatch_queue_t serial_queue = dispatch_queue_create("watch_dir_events_queue", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(streamRef, serial_queue);

    if (!FSEventStreamStart(streamRef))
    {
        fprintf(stderr, "failed to start stream for filesystem events");
        FSEventStreamRelease(streamRef);
        CFRelease(paths);
        CFRelease(path);
        dispatch_release(serial_queue);
        return false;
    }

    wait_for_ctrl_c();
    FSEventStreamStop(streamRef);
    FSEventStreamInvalidate(streamRef);
    dispatch_release(serial_queue);
    FSEventStreamRelease(streamRef);
    CFRelease(paths);
    CFRelease(path);
    return true;
}
