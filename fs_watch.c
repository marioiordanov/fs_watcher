#include <CoreServices/CoreServices.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <signal.h>
#include <pthread.h>

const CFTimeInterval latency = 0.5;

static void translate_fs_event_flag(FSEventStreamEventFlags f)
{
    struct
    {
        FSEventStreamEventFlags flag;
        char *translation;
    } bits[] = {
        {kFSEventStreamEventFlagItemIsDir, "Folder"},
        {kFSEventStreamEventFlagItemIsFile, "File"},
        {kFSEventStreamEventFlagItemXattrMod, "ExtendedAttributesChanged"},
        {kFSEventStreamEventFlagItemChangeOwner, "OwnerChanged"},
        {kFSEventStreamEventFlagItemFinderInfoMod, "FinderInfoChanged"},
        {kFSEventStreamEventFlagItemModified, "ContentsChanged"},
        {kFSEventStreamEventFlagItemRenamed, "Renamed"},
        {kFSEventStreamEventFlagItemInodeMetaMod, "MetadataChanged"},
        {kFSEventStreamEventFlagItemRemoved, "Removed"},
        {kFSEventStreamEventFlagItemCreated, "Created"}};

    bool found = false;
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
    {
        if (bits[i].flag & f)
        {
            found = true;
            printf("%s ", bits[i].translation);
        }
    }

    if (!found)
    {
        printf("Unrecognized event %d", f);
    }
}

bool does_object_exist(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static inline bool has_flag(FSEventStreamEventFlags flag, FSEventStreamEventFlags bit)
{
    return (flag & bit) != 0;
}

bool is_DS_Store_path(const char *path)
{
    if (!path)
        return false;

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    return strcmp(base, ".DS_Store") == 0;
}

/// object_flag shows if it is file or folder
bool is_object_renamed(FSEventStreamEventFlags object_flag, size_t events_count, const FSEventStreamEventFlags *eventFlags, const FSEventStreamEventId *eventIds, const char *const *paths)
{
    if (events_count != 2)
    {
        return false;
    }

    // first events id is x, seconds is x+1
    if (eventIds[0] + 1 != eventIds[1])
    {
        return false;
    }

    for (size_t i = 0; i < events_count; i++)
    {
        bool is_file = has_flag(eventFlags[0], object_flag);
        bool is_renamed = has_flag(eventFlags[0], kFSEventStreamEventFlagItemRenamed);

        if (!(is_file && is_renamed))
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

bool is_object_removed(FSEventStreamEventFlags object_flag, FSEventStreamEventFlags flag, const char *file_path)
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

bool is_file_removed(FSEventStreamEventFlags flag, const char *file_path)
{
    return is_object_removed(kFSEventStreamEventFlagItemIsFile, flag, file_path);
}

bool is_folder_removed(FSEventStreamEventFlags flag, const char *file_path)
{
    return is_object_removed(kFSEventStreamEventFlagItemIsDir, flag, file_path);
}

bool is_object_created(FSEventStreamEventFlags object_flag, FSEventStreamEventFlags flag, const char *file_path)
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

bool is_file_created(FSEventStreamEventFlags flag, const char *file_path)
{
    return is_object_created(kFSEventStreamEventFlagItemIsFile, flag, file_path);
}

bool is_folder_created(FSEventStreamEventFlags flag, const char *file_path)
{
    return is_object_created(kFSEventStreamEventFlagItemIsDir, flag, file_path);
}

bool is_object_added(FSEventStreamEventFlags object_flag, FSEventStreamEventFlags flag, const char *file_path)
{
    if (!has_flag(flag, object_flag))
    {
        return false;
    }

    if (!has_flag(flag, kFSEventStreamEventFlagItemModified) &&has_flag(flag, kFSEventStreamEventFlagItemRenamed) && does_object_exist(file_path))
    {
        return true;
    }

    return false;
}

bool is_file_added(FSEventStreamEventFlags flag, const char *file_path)
{
    return is_object_added(kFSEventStreamEventFlagItemIsFile, flag, file_path);
}

bool is_folder_added(FSEventStreamEventFlags flag, const char *file_path)
{
    return is_object_added(kFSEventStreamEventFlagItemIsDir, flag, file_path);
}

bool is_file_modified(FSEventStreamEventFlags flag, const char *file_path)
{
    if (has_flag(flag, kFSEventStreamEventFlagItemIsFile) && has_flag(flag, kFSEventStreamEventFlagItemModified) && does_object_exist(file_path))
    {
        return true;
    }

    return false;
}

void stream_callback(ConstFSEventStreamRef streamRef, void *clientCallBackInfo, size_t numEvents, void *eventPaths, const FSEventStreamEventFlags *eventFlags, const FSEventStreamEventId *eventIds)
{
    (void)streamRef;
    (void)clientCallBackInfo;
    const char *const *paths = (const char *const *)eventPaths;

    if (numEvents == 1)
    {
        if (is_DS_Store_path(paths[0]))
        {
            printf("ds store'\n");
            return;
        }

        if (is_file_removed(eventFlags[0], paths[0]))
        {
            printf("%s was removed\n", paths[0]);
        }
        else if (is_folder_removed(eventFlags[0], paths[0]))
        {
            printf("Folder %s was removed\n", paths[0]);
        }
        else if (is_file_created(eventFlags[0], paths[0]))
        {
            printf("File at path %s was created\n", paths[0]);
        }
        else if (is_folder_created(eventFlags[0], paths[0]))
        {
            printf("Folder at path %s was created\n", paths[0]);
        }
        else if (is_file_added(eventFlags[0], paths[0]))
        {
            printf("File %s was added\n", paths[0]);
        }
        else if (is_folder_added(eventFlags[0], paths[0]))
        {
            printf("Folder %s was added \n", paths[0]);
        }
        else if (is_file_modified(eventFlags[0], paths[0]))
        {
            printf("File %s was modified\n", paths[0]);
        }
    }
    else
    {
        if (is_file_renamed(numEvents, eventFlags, eventIds, paths))
        {
            printf("File renamed from %s to %s\n", paths[0], paths[1]);
        }
        else if (is_folder_renamed(numEvents, eventFlags, eventIds, paths))
        {
            printf("Folder renamed from %s to %s\n", paths[0], paths[1]);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2 && !does_object_exist(argv[1]))
        return 1;
    const char *dir_path = argv[1];

    CFStringRef path = CFStringCreateWithCString(NULL, dir_path, kCFStringEncodingUTF8);
    if (!path)
    {
        fprintf(stderr, "failed to create CFString for path\n");
        return 1;
    }
    CFArrayRef paths = CFArrayCreate(NULL, (const void *[]){path}, 1, &kCFTypeArrayCallBacks);
    if (paths == NULL)
    {
        fprintf(stderr, "failed to create CFArray for paths\n");
        CFRelease(path);
        return 1;
    }
    FSEventStreamCreateFlags flags = kFSEventStreamCreateFlagFileEvents;
    FSEventStreamRef streamRef = FSEventStreamCreate(NULL, stream_callback, NULL, paths, kFSEventStreamEventIdSinceNow, latency, flags);

    if (streamRef == NULL)
    {
        fprintf(stderr, "failed to create stream for filesystem events");
        CFRelease(paths);
        CFRelease(path);
        return 1;
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
        return 1;
    }

    printf("press key to exit\n");
    getchar();

    FSEventStreamInvalidate(streamRef);
    FSEventStreamStop(streamRef);
    dispatch_release(serial_queue);
    FSEventStreamRelease(streamRef);
    CFRelease(paths);
    CFRelease(path);
}
