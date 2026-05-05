#include "watcher.h"

#include <CoreServices/CoreServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fts.h>
#include <stdio.h>
#include <string.h>
#include "util.h"
#include "protocol.h"

const size_t RENAMED_EVENTS_COUNT = 2;
const size_t REPLACED_EVENTS_COUNT = 2;
const size_t FILE_MODIFIED_VIA_TMP_FILE_EVENTS_COUNT = 2;
const size_t FILE_MODIFIED_VIA_REPLACE = 2;
const size_t FILE_MODIFIED_SAVE_SAFE_EVENTS_COUNT = 3;

typedef struct
{
    const char **excluded_names;
    size_t excluded_count;
} WatcherConfig;

typedef struct
{
    char path[PATH_MAX + 1];
    ino_t inode;
    FSEventStreamEventFlags flags;
    FSEventStreamEventId eventId;
} EventData;

static bool is_path_excluded(const char *path, const WatcherConfig *config)
{
    if (!path || !config || config->excluded_count == 0)
        return false;

    char path_with_slash[PATH_MAX + 2] = {0};
    size_t len = strlen(path);
    memcpy(path_with_slash, path, len);
    path_with_slash[len] = '/';

    for (size_t i = 0; i < config->excluded_count; i++)
    {
        if (strstr(path_with_slash, config->excluded_names[i]) != NULL)
        {
            return true;
        }
    }

    return false;
}

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
        {kFSEventStreamEventFlagItemCreated, "Created"},
        {kFSEventStreamEventFlagOwnEvent, "OwnEvent"},
        {kFSEventStreamEventFlagItemCloned, "Cloned"}};

    bool found = false;
    unsigned int current = 0;
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
    {
        if (bits[i].flag & f)
        {
            found = true;
            current |= bits[i].flag;
            fprintf(stderr, "%s ", bits[i].translation);
        }
    }

    if (!found)
    {
        fprintf(stderr, "Unrecognized event %d", f);
    }

    if (current != f)
    {
        fprintf(stderr, "Not all pieces handled %d", f - current);
    }
}

bool load_event_data_for_index_if_not_excluded(CFArrayRef array, CFIndex index, const FSEventStreamEventFlags *eventFlags, const FSEventStreamEventId *eventIds, const WatcherConfig *config, EventData *const out)
{
    CFDictionaryRef pathInodeDictRef = (CFDictionaryRef)CFArrayGetValueAtIndex(array, index);
    CFStringRef pathRef = CFDictionaryGetValue(pathInodeDictRef, kFSEventStreamEventExtendedDataPathKey);

    if (pathRef == NULL)
    {
        return false;
    }

    CFNumberRef inodeRef = CFDictionaryGetValue(pathInodeDictRef, kFSEventStreamEventExtendedFileIDKey);
    if (inodeRef == NULL)
    {
        return false;
    }

    char path[PATH_MAX + 1] = {0};
    ino_t inode;

    if (!CFStringGetCString(pathRef, path, sizeof(path), kCFStringEncodingUTF8))
    {
        return false;
    }

    if (is_DS_Store_path(path) || is_path_excluded(path, config))
    {
        return false;
    }

    if (!CFNumberGetValue(inodeRef, kCFNumberLongLongType, &inode))
    {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->inode = inode;
    memcpy(out->path, path, sizeof(out->path));
    out->flags = eventFlags[index];
    out->eventId = eventIds[index];

    return true;
}

static inline bool has_flag(FSEventStreamEventFlags flag, FSEventStreamEventFlags bit)
{
    return (flag & bit) != 0;
}

// the link between the 3 events is the following: First and second have different names, but same inodes. Because first macOS creates a backup
// Then second and third have different names and inodes, but the relation between first and last is that they have equal names.
bool check_and_order_if_file_is_modified_via_tmp_file_using_3_events(const EventData *(*p_arr)[3])
{
    const EventData *const first = (*p_arr)[0];
    const EventData *const second = (*p_arr)[1];
    const EventData *const third = (*p_arr)[2];

    if (first == NULL || second == NULL || third == NULL)
        return false;

    const EventData *arr[3] = {0};
    FSEventStreamEventFlags renamed_and_cloned = kFSEventStreamEventFlagItemRenamed | kFSEventStreamEventFlagItemCloned | kFSEventStreamEventFlagItemIsFile;
    FSEventStreamEventFlags renamed_removed_and_cloned = renamed_and_cloned | kFSEventStreamEventFlagItemRemoved;

    // first order the events: [ (original name + old inode) , (sb name + old inode), (original name + new inode)]

    if ((first->inode != second->inode) && (first->inode != third->inode))
    {
        arr[2] = first; // this means that first inode is different from the other 2, so its the new inode

        if (strcmp(first->path, second->path) == 0)
        {
            arr[0] = second;
            arr[1] = third;
        }
        else
        {
            arr[0] = third;
            arr[1] = second;
        }
    }
    else
    { // first inode is equal to second or third. Therefore it is the old inode. Now we need to find which is the temp file
        if (strcmp(first->path, second->path) == 0)
        {
            arr[0] = first;
            arr[1] = third;
            arr[2] = second;
        }
        else if (strcmp(first->path, third->path) == 0)
        {
            arr[0] = first;
            arr[1] = second;
            arr[2] = third;
        }
        else
        {
            arr[1] = first;
            if (first->inode == second->inode)
            {
                arr[0] = second;
                arr[2] = third;
            }
            else
            {
                arr[0] = third;
                arr[2] = second;
            }
        }
    }

    // now events are ordered, lets check the attributes
    if (!has_flag(arr[0]->flags, renamed_and_cloned))
        return false;
    if (!has_flag(arr[1]->flags, renamed_removed_and_cloned))
        return false;
    if (!has_flag(arr[2]->flags, renamed_and_cloned))
        return false;

    (*p_arr)[0] = arr[0];
    (*p_arr)[1] = arr[1];
    (*p_arr)[2] = arr[2];

    return true;
}
bool is_file_modified_via_tmp_file_with_3_events(const EventData *const first, const EventData *const second, const EventData *const third)
{
    if (first == NULL || second == NULL || third == NULL)
        return false;
    const EventData *arr[3] = {0};
    FSEventStreamEventFlags renamed_and_cloned = kFSEventStreamEventFlagItemRenamed | kFSEventStreamEventFlagItemCloned | kFSEventStreamEventFlagItemIsFile;
    FSEventStreamEventFlags renamed_removed_and_cloned = renamed_and_cloned | kFSEventStreamEventFlagItemRemoved;

    // first order the events: [ (original name + old inode) , (sb name + old inode), (original name + new inode)]

    if ((first->inode != second->inode) && (first->inode != third->inode))
    {
        arr[2] = first; // this means that first inode is different from the other 2, so its the new inode

        if (strcmp(first->path, second->path) == 0)
        {
            arr[0] = second;
            arr[1] = third;
        }
        else
        {
            arr[0] = third;
            arr[1] = second;
        }
    }
    else
    { // first inode is equal to second or third. Therefore it is the old inode. Now we need to find which is the temp file
        if (strcmp(first->path, second->path) == 0)
        {
            arr[0] = first;
            arr[1] = third;
            arr[2] = second;
        }
        else if (strcmp(first->path, third->path) == 0)
        {
            arr[0] = first;
            arr[1] = second;
            arr[2] = third;
        }
        else
        {
            arr[1] = first;
            if (first->inode == second->inode)
            {
                arr[0] = second;
                arr[2] = third;
            }
            else
            {
                arr[0] = third;
                arr[2] = second;
            }
        }
    }

    // now events are ordered, lets check the attributes
    if (!has_flag(arr[0]->flags, renamed_and_cloned))
        return false;
    if (!has_flag(arr[1]->flags, renamed_removed_and_cloned))
        return false;
    if (!has_flag(arr[2]->flags, renamed_and_cloned))
        return false;

    return true;
}

bool is_file_modified_via_2_events(const EventData *const first, const EventData *const second)
{
    if (first == NULL || second == NULL)
        return false;

    if (strcmp(first->path, second->path) != 0)
        return false;

    if (first->inode != second->inode)
        return false;

    bool first_exists = does_object_with_inode_exist(first->path, first->inode);
    bool second_exists = does_object_with_inode_exist(second->path, second->inode);
    if ((!first_exists && !second_exists) || (first_exists && second_exists))
        return false;

    FSEventStreamEventFlags renamed_and_cloned = kFSEventStreamEventFlagItemRenamed | kFSEventStreamEventFlagItemCloned | kFSEventStreamEventFlagItemIsFile;

    if (!has_flag(first->flags, renamed_and_cloned))
        return false;
    if (!has_flag(second->flags, renamed_and_cloned))
        return false;

    return true;
}

bool is_file_modified_via_tmp_file(const EventData *const current, const EventData *const next)
{
    if (current == NULL || next == NULL)
        return false;

    if (strcmp(current->path, next->path) != 0)
        return false;

    if (!has_flag(current->flags, kFSEventStreamEventFlagItemIsFile))
        return false;
    if (!has_flag(next->flags, kFSEventStreamEventFlagItemIsFile))
        return false;
    if (!has_flag(current->flags, kFSEventStreamEventFlagItemRemoved))
        return false;
    if (!has_flag(next->flags, kFSEventStreamEventFlagItemRenamed))
        return false;

    return true;
}

typedef bool (*send_object_fn)(ObjectType object_type, const char *path, ino_t inode);

static void send_folder_contents_recursive(const char *folder_path, send_object_fn sender, const WatcherConfig *config)
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

        if (is_path_excluded(node->fts_path, config))
        {
            if (node->fts_info == FTS_D)
                fts_set(tree, node, FTS_SKIP);
            continue;
        }

        if (node->fts_info == FTS_F)
        {
            sender(OBJECT_FILE, node->fts_path, node->fts_statp->st_ino);
        }
        else if (node->fts_info == FTS_D)
        {
            sender(OBJECT_FOLDER, node->fts_path, node->fts_statp->st_ino);
        }
    }

    fts_close(tree);
}

static void send_folder_contents_renamed(const char *oldFolderPath, const char *currentFolderPath, const WatcherConfig *config)
{
    if (!oldFolderPath || !currentFolderPath)
        return;

    char *paths[] = {(char *)currentFolderPath, NULL};

    FTS *tree = fts_open(paths, FTS_NOCHDIR | FTS_PHYSICAL, NULL);
    if (tree == NULL)
    {
        fprintf(stderr, "failed to open folder tree: %s\n", currentFolderPath);
        return;
    }

    char oldPath[PATH_MAX + 1] = {0};

    FTSENT *node = NULL;
    while ((node = fts_read(tree)) != NULL)
    {
        if (node->fts_level == 0)
            continue;
        if (is_DS_Store_path(node->fts_path))
            continue;

        if (is_path_excluded(node->fts_path, config))
        {
            if (node->fts_info == FTS_D)
                fts_set(tree, node, FTS_SKIP);
            continue;
        }

        const char *const currentPath = node->fts_path;
        size_t suffixLen = strlen(currentPath) - strlen(currentFolderPath);
        strncpy(oldPath, oldFolderPath, sizeof(oldPath) - 1);
        strncpy(&oldPath[strlen(oldFolderPath)], &currentPath[strlen(currentFolderPath)], suffixLen);

        if (node->fts_info == FTS_F)
        {
            send_object_renamed(OBJECT_FILE, oldPath, currentPath, node->fts_statp->st_ino);
        }
        else if (node->fts_info == FTS_D)
        {
            send_object_renamed(OBJECT_FOLDER, oldPath, currentPath, node->fts_statp->st_ino);
        }
    }

    fts_close(tree);
}

bool is_object_renamed(FSEventStreamEventFlags objectTypeFlag, const EventData *const current, const EventData *const next)
{
    if (current == NULL || next == NULL)
        return false;

    // first events id is x, second is x+1
    if (current->eventId + 1 != next->eventId)
        return false;

    if (!has_flag(current->flags, objectTypeFlag))
        return false;
    if (!has_flag(next->flags, objectTypeFlag))
        return false;
    if (!has_flag(current->flags, kFSEventStreamEventFlagItemRenamed))
        return false;
    if (!has_flag(next->flags, kFSEventStreamEventFlagItemRenamed))
        return false;

    // rename doesn't change the inode
    if (current->inode != next->inode)
        return false;

    // the second path is the new name
    if (!does_object_with_inode_exist(next->path, next->inode))
        return false;

    return true;
}

bool is_file_renamed(const EventData *const current, const EventData *const next)
{
    return is_object_renamed(kFSEventStreamEventFlagItemIsFile, current, next);
}

bool is_folder_renamed(const EventData *const current, const EventData *const next)
{
    return is_object_renamed(kFSEventStreamEventFlagItemIsDir, current, next);
}

static bool is_object_removed(FSEventStreamEventFlags object_flag, const EventData *const event)
{
    bool is_renamed = has_flag(event->flags, kFSEventStreamEventFlagItemRenamed);
    bool is_deleted = has_flag(event->flags, kFSEventStreamEventFlagItemRemoved);

    if (!has_flag(event->flags, object_flag))
    {
        return false;
    }

    if (is_renamed && !does_object_exist(event->path))
    {
        return true;
    }

    if (is_deleted)
    {
        return true;
    }

    return false;
}

static bool is_file_removed(const EventData *const event)
{
    return is_object_removed(kFSEventStreamEventFlagItemIsFile, event);
}

static bool is_folder_removed(const EventData *const event)
{
    return is_object_removed(kFSEventStreamEventFlagItemIsDir, event);
}

static bool is_object_created(FSEventStreamEventFlags object_flag, const EventData *const event)
{
    if (!has_flag(event->flags, object_flag))
        return false;
    if (!has_flag(event->flags, kFSEventStreamEventFlagItemCreated))
        return false;
    if (!does_object_exist(event->path))
        return false;

    return true;
}

static bool is_file_created(const EventData *const event)
{
    return is_object_created(kFSEventStreamEventFlagItemIsFile, event);
}

static bool is_folder_created(const EventData *const event)
{
    return is_object_created(kFSEventStreamEventFlagItemIsDir, event);
}

static bool is_object_added(FSEventStreamEventFlags objectFlag, const EventData *const event)
{
    if (event == NULL)
        return false;
    if (!has_flag(event->flags, objectFlag))
        return false;

    if (!has_flag(event->flags, kFSEventStreamEventFlagItemRenamed))
        return false;
    if (has_flag(event->flags, kFSEventStreamEventFlagItemModified))
        return false;
    if (!does_object_exist(event->path))
        return false;

    return true;
}

static bool is_file_added(const EventData *const event)
{
    return is_object_added(kFSEventStreamEventFlagItemIsFile, event);
}

static bool is_folder_added(const EventData *const event)
{
    return is_object_added(kFSEventStreamEventFlagItemIsDir, event);
}

static bool is_file_modified(const EventData *const current)
{
    if (current == NULL)
        return false;

    if (!has_flag(current->flags, kFSEventStreamEventFlagItemIsFile))
        return false;
    if (!has_flag(current->flags, kFSEventStreamEventFlagItemModified))
        return false;
    if (!does_object_exist(current->path))
        return false;

    return true;
}

static size_t handle_renamed_object(const EventData *const current, const EventData *const next, const WatcherConfig *config)
{
    size_t consumedEvents = 0;

    if (is_file_renamed(current, next))
    {
        consumedEvents = RENAMED_EVENTS_COUNT;
        send_object_renamed(OBJECT_FILE, current->path, next->path, current->inode);
    }
    else if (is_folder_renamed(current, next))
    {
        consumedEvents = RENAMED_EVENTS_COUNT;
        send_object_renamed(OBJECT_FOLDER, current->path, next->path, current->inode);
        send_folder_contents_renamed(current->path, next->path, config);
    }

    return consumedEvents;
}

static size_t handle_removed_object(const EventData *const event)
{
    size_t consumedEvents = 0;

    if (is_file_removed(event))
    {
        consumedEvents = 1;
        send_object_removed(OBJECT_FILE, event->path, event->inode);
    }
    else if (is_folder_removed(event))
    {
        consumedEvents = 1;
        send_object_removed(OBJECT_FOLDER, event->path, event->inode);
    }

    return consumedEvents;
}

static size_t handle_created_object(const EventData *const event, const WatcherConfig *config)
{
    size_t consumedEvents = 0;

    if (is_file_created(event))
    {
        consumedEvents = 1;
        send_object_created(OBJECT_FILE, event->path, event->inode);
    }
    else if (is_folder_created(event))
    {
        consumedEvents = 1;
        send_object_created(OBJECT_FOLDER, event->path, event->inode);
        send_folder_contents_recursive(event->path, send_object_created, config);
    }

    return consumedEvents;
}

static size_t handle_added_object(const EventData *const event, const WatcherConfig *config)
{
    size_t consumedEvents = 0;

    if (is_file_added(event))
    {
        consumedEvents = 1;
        send_object_added(OBJECT_FILE, event->path, event->inode);
    }
    else if (is_folder_added(event))
    {
        consumedEvents = 1;
        send_object_added(OBJECT_FOLDER, event->path, event->inode);
        send_folder_contents_recursive(event->path, send_object_added, config);
    }

    return consumedEvents;
}

static size_t handle_modified_file(const EventData *const first, const EventData *const second, const EventData *const third)
{
    size_t consumedEvents = 0;

    const EventData *arr[3] = {first, second, third};
    if (check_and_order_if_file_is_modified_via_tmp_file_using_3_events(&arr))
    {
        consumedEvents = FILE_MODIFIED_SAVE_SAFE_EVENTS_COUNT;
        send_object_modified(OBJECT_FILE, arr[0]->path, arr[0]->inode, arr[2]->inode);
    }
    else if (is_file_modified_via_2_events(first, second))
    {
        consumedEvents = FILE_MODIFIED_VIA_REPLACE;
        send_object_modified(OBJECT_FILE, first->path, first->inode, second->inode);
    }
    else if (is_file_modified_via_tmp_file(first, second))
    {
        consumedEvents = FILE_MODIFIED_VIA_TMP_FILE_EVENTS_COUNT;
        send_object_modified(OBJECT_FILE, second->path, first->inode, second->inode);
    }
    else if (is_file_modified(first))
    {
        consumedEvents = 1;
        send_object_modified(OBJECT_FILE, first->path, first->inode, first->inode);
    }

    return consumedEvents;
}

// replace events have the same path, different inodes and file renamed event flags
static bool is_object_replaced(FSEventStreamCreateFlags objectTypeFlag, const EventData *const current, const EventData *const next)
{
    if (current == NULL || next == NULL)
        return false;

    if (strcmp(current->path, next->path) != 0)
        return false;

    const char *path = next->path;

    if (!has_flag(current->flags, objectTypeFlag))
        return false;
    if (!has_flag(next->flags, objectTypeFlag))
        return false;

    // both events are renamed only
    if (!has_flag(current->flags, kFSEventStreamEventFlagItemRenamed))
        return false;
    if (!has_flag(next->flags, kFSEventStreamEventFlagItemRenamed))
        return false;
    if (has_flag(current->flags, kFSEventStreamEventFlagItemRemoved))
        return false;

    // (path + oldInode) does not exist, (path + newInode) exist
    if (does_object_with_inode_exist(path, current->inode))
        return false;
    if (!does_object_with_inode_exist(path, next->inode))
        return false;

    return true;
}

static size_t handle_replaced_object(const EventData *const current, const EventData *const next, const WatcherConfig *config)
{
    size_t consumedEvents = 0;

    if (is_object_replaced(kFSEventStreamEventFlagItemIsFile, current, next))
    {
        consumedEvents = REPLACED_EVENTS_COUNT;
        send_object_replaced(OBJECT_FILE, next->path, current->inode, next->inode);
    }
    else if (is_object_replaced(kFSEventStreamEventFlagItemIsDir, current, next))
    {
        consumedEvents = REPLACED_EVENTS_COUNT;
        send_object_replaced(OBJECT_FOLDER, next->path, current->inode, next->inode);
        send_folder_contents_recursive(next->path, send_object_added, config);
    }

    return consumedEvents;
}

static void stream_callback_with_CF_types(ConstFSEventStreamRef streamRef, void *clientCallBackInfo, size_t numEvents, void *eventPaths, const FSEventStreamEventFlags *eventFlags, const FSEventStreamEventId *eventIds)
{
    (void)streamRef;
    const WatcherConfig *config = (const WatcherConfig *)clientCallBackInfo;
    CFArrayRef paths = (CFArrayRef)eventPaths;

    enum
    {
        window_len = 3
    };
    EventData window[window_len] = {0};
    EventData *window_ptrs[window_len] = {0};
    for (size_t i = 0; i < window_len; i++)
    {
        window_ptrs[i] = &window[i];
    }

    fprintf(stderr, "events count %lu\n", numEvents);
    size_t elements_to_load = window_len;
    size_t w_idx = 0;

    for (size_t i = 0; i < numEvents;)
    {
        size_t k = i;
        for (; k < i + elements_to_load; k++)
        {
            if (k < numEvents)
            {
                if (load_event_data_for_index_if_not_excluded(paths, k, eventFlags, eventIds, config, window_ptrs[w_idx % window_len]))
                {
                    w_idx++;
                }
                else
                {
                    i++;
                }
            }
            else
            {
                window_ptrs[w_idx % window_len] = NULL;
                w_idx++;
            }
        }

        for (size_t t = 0; t < window_len; t++)
        {
            EventData *current = window_ptrs[(w_idx + t) % window_len];

            if (current != NULL)
            {
                fprintf(stderr, "%s %llu %llu ", current->path, current->inode, current->eventId);
                translate_fs_event_flag(current->flags);
                fprintf(stderr, "\n");
            }
        }

        size_t consumed = 0;

        if (consumed == 0)
        { // handle modified file
            consumed = handle_modified_file(window_ptrs[w_idx % window_len], window_ptrs[(w_idx + 1) % window_len], window_ptrs[(w_idx + 2) % window_len]);
        }
        if (consumed == 0)
        { // handle renamed file/folder
            consumed = handle_renamed_object(window_ptrs[w_idx % window_len], window_ptrs[(w_idx + 1) % window_len], config);
        }
        if (consumed == 0)
        {
            consumed = handle_replaced_object(window_ptrs[w_idx % window_len], window_ptrs[(w_idx + 1) % window_len], config);
        }
        if (consumed == 0)
        { // handle removed file/folder
            consumed = handle_removed_object(window_ptrs[w_idx % window_len]);
        }
        if (consumed == 0)
        { // handle created file/folder
            consumed = handle_created_object(window_ptrs[w_idx % window_len], config);
        }
        if (consumed == 0)
        { // handle added file/folder
            consumed = handle_added_object(window_ptrs[w_idx % window_len], config);
        }

        if (consumed == 0)
        {
            consumed = 1;
        }

        i += elements_to_load;
        elements_to_load = consumed;
    }
}

bool run_watcher(const char *dir_path, double latency, const char **excluded_names, size_t excluded_count)
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

    WatcherConfig config = {
        .excluded_names = excluded_names,
        .excluded_count = excluded_count,
    };

    WatcherConfig *ptr_config = config.excluded_count == 0 ? NULL : &config;

    FSEventStreamContext ctx = {
        .version = 0,
        .info = ptr_config,
        .retain = NULL,
        .release = NULL,
        .copyDescription = NULL,
    };

    FSEventStreamCreateFlags flags = kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagUseExtendedData | kFSEventStreamCreateFlagUseCFTypes;
    FSEventStreamRef streamRef = FSEventStreamCreate(NULL, stream_callback_with_CF_types, &ctx, paths, kFSEventStreamEventIdSinceNow, latency, flags);

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

#ifdef RUN_TESTS
void test_ordering_of_modify_with_3_events(void)
{
    FSEventStreamEventFlags renamed_cloned =
        kFSEventStreamEventFlagItemRenamed |
        kFSEventStreamEventFlagItemCloned |
        kFSEventStreamEventFlagItemIsFile;
    FSEventStreamEventFlags renamed_removed_cloned =
        renamed_cloned | kFSEventStreamEventFlagItemRemoved;

    EventData a = {.inode = 100, .flags = renamed_cloned, .eventId = 10};
    EventData b = {.inode = 100, .flags = renamed_removed_cloned, .eventId = 17};
    EventData c = {.inode = 200, .flags = renamed_cloned, .eventId = 30};
    strncpy(a.path, "data/file.txt", PATH_MAX);
    strncpy(b.path, "data/file.txt.sb-5c789d70-vKNEuX", PATH_MAX);
    strncpy(c.path, "data/file.txt", PATH_MAX);

    // all 6 permutations
    const EventData *perms[6][3] = {
        {&a, &b, &c},
        {&a, &c, &b},
        {&b, &a, &c},
        {&b, &c, &a},
        {&c, &a, &b},
        {&c, &b, &a},
    };

    for (int i = 0; i < 6; i++)
    {
        const EventData *arr[3] = {perms[i][0], perms[i][1], perms[i][2]};
        bool ok = check_and_order_if_file_is_modified_via_tmp_file_using_3_events(&arr);

        assert(ok);
        assert(strcmp(arr[0]->path, "data/file.txt") == 0);                    // A
        assert(strcmp(arr[1]->path, "data/file.txt.sb-5c789d70-vKNEuX") == 0); // B
        assert(strcmp(arr[2]->path, "data/file.txt") == 0);                    // C
        assert(arr[0]->inode == 100);
        assert(arr[1]->inode == 100);
        assert(arr[2]->inode == 200);
    }

    printf("%s completed successfully\n", __func__);
}
#endif
