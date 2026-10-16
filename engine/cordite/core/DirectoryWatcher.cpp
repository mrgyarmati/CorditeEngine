#include "cordite/core/DirectoryWatcher.h"

#include "cordite/core/FileSystem.h"

#include <condition_variable>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <utility>

namespace cordite {

std::map<std::string, FileStamp> stampFiles(const std::filesystem::path& directory) {
    std::map<std::string, FileStamp> files;
    std::error_code error;
    std::filesystem::recursive_directory_iterator entry(
        directory, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    for (; !error && entry != end; entry.increment(error)) {
        std::error_code statusError;
        if (entry->is_directory(statusError)) {
            if (pathToUtf8(entry->path().filename()).starts_with('.')) {
                entry.disable_recursion_pending();
            }
            continue;
        }
        // A file whose name ends in .tmp is on its way to being written: writeBinaryFile()
        // writes such a sibling and renames it, and a look between the two would take it
        // for a file that came and went.
        if (!entry->is_regular_file(statusError) || pathToUtf8(entry->path().filename()).ends_with(".tmp")) {
            continue;
        }
        FileStamp stamp;
        stamp.size = entry->file_size(statusError);
        if (!statusError) {
            stamp.written = entry->last_write_time(statusError);
        }
        // A file that went away while it was looked at is not there.
        if (!statusError) {
            files.emplace(pathToUtf8(entry->path().lexically_relative(directory)), stamp);
        }
    }
    return files;
}

std::vector<std::string> changedFiles(const std::map<std::string, FileStamp>& before,
                                      const std::map<std::string, FileStamp>& after) {
    std::vector<std::string> changed;
    auto old = before.begin();
    auto now = after.begin();
    // Both are sorted by path: one walk through the two finds what differs.
    while (old != before.end() || now != after.end()) {
        if (now == after.end() || (old != before.end() && old->first < now->first)) {
            changed.push_back(old->first);
            ++old;
        } else if (old == before.end() || now->first < old->first) {
            changed.push_back(now->first);
            ++now;
        } else {
            if (!(old->second == now->second)) {
                changed.push_back(now->first);
            }
            ++old;
            ++now;
        }
    }
    return changed;
}

struct DirectoryWatcher::Impl {
    DirectoryWatcherDesc desc;

    std::mutex mutex;
    std::condition_variable wake;
    // Guarded by the mutex.
    bool stopping = false;
    std::map<std::string, FileStamp> files;
    std::set<std::string> changes;
    /// Counts rebaseline(): a look that began before one is out of date when it ends.
    uint64_t baselines = 0;

    std::thread thread;

    void look() {
        std::unique_lock lock(mutex);
        while (true) {
            wake.wait_for(lock, desc.interval, [this] { return stopping; });
            if (stopping) {
                return;
            }
            const uint64_t lookedAfter = baselines;
            lock.unlock();
            std::map<std::string, FileStamp> now = stampFiles(desc.directory);
            lock.lock();
            if (lookedAfter != baselines) {
                continue;
            }
            for (std::string& path : changedFiles(files, now)) {
                changes.insert(std::move(path));
            }
            files = std::move(now);
        }
    }
};

DirectoryWatcher::DirectoryWatcher(DirectoryWatcherDesc desc) : m_impl(std::make_unique<Impl>()) {
    m_impl->desc = std::move(desc);
    m_impl->files = stampFiles(m_impl->desc.directory);
    m_impl->thread = std::thread([impl = m_impl.get()] { impl->look(); });
}

DirectoryWatcher::~DirectoryWatcher() {
    {
        const std::scoped_lock lock(m_impl->mutex);
        m_impl->stopping = true;
    }
    m_impl->wake.notify_all();
    m_impl->thread.join();
}

std::vector<std::string> DirectoryWatcher::takeChanges() {
    const std::scoped_lock lock(m_impl->mutex);
    std::vector<std::string> changes(m_impl->changes.begin(), m_impl->changes.end());
    m_impl->changes.clear();
    return changes;
}

void DirectoryWatcher::rebaseline() {
    std::map<std::string, FileStamp> now = stampFiles(m_impl->desc.directory);
    const std::scoped_lock lock(m_impl->mutex);
    m_impl->files = std::move(now);
    m_impl->changes.clear();
    ++m_impl->baselines;
}

} // namespace cordite
