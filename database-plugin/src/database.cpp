#include "storage.hpp"

#include <prequel/container/default_allocator.hpp>
#include <prequel/simple_file_format.hpp> // just for magic_header... FIXME move it
#include <prequel/transaction_engine.hpp>
#include <prequel/vfs.hpp>

#include <pybind11/pybind11.h>

#include <memory>
#include <mutex>
#include <sstream>

namespace blabber {

namespace py = pybind11;

/*
 * The database is the top level interface exposed to clients (i.e. the python code).
 *
 * All public member functions run in the context of a transaction and are therefore atomic.
 */
class database {
public:
    explicit database(const std::string& path, u32 cache_blocks);
    ~database();

    database(const database&) = delete;
    database& operator=(const database&) = delete;

    // Todo: flag for disabling "sync on commit"

    u64 create_post(const std::string& user, const std::string& title, const std::string& content);
    bool create_comment(u64 post_id, const std::string& user, const std::string& content);
    py::list fetch_frontpage(size_t max_posts);
    py::object fetch_post(u64 post_id, size_t max_comments);

    // Called on a clean shutdown: performs a checkpoint and erases the journal.
    void finish();

    std::string dump();

private:
    static constexpr const char FILE_FORMAT_MAGIC[] = "BLABBER_DB";
    static constexpr u32 FILE_FORMAT_VERSION = 1;

    // At offset 0 in the file.
    struct file_header {
        prequel::magic_header magic;
        u32 version = 0;

        static constexpr auto get_binary_format() {
            return prequel::binary_format(&file_header::magic, &file_header::version);
        }
    };

    // Full content of the first block.
    struct master_block {
        file_header header;
        prequel::default_allocator::anchor alloc;
        storage::anchor store;

        static constexpr auto get_binary_format() {
            return prequel::binary_format(&master_block::header, &master_block::alloc,
                                          &master_block::store);
        }
    };

private:
    void open();
    void init_master_block();
    void check_master_block();

    static void check_header(const file_header& header);

    /*
     * Unlocks the python GIL and locks our mutex, then executes `fn`.
     */
    template<typename Func>
    void exec(Func&& fn);

    /*
     * Like exec, but also starts a transaction. The transaction will be committed
     * if `fn` returns without an exception, and will be rolled back otherwise.
     */
    template<typename Func>
    void exec_transaction(Func&& fn);

private:
    // These values are constant after construction.
    std::string m_database_path;
    std::string m_journal_path;
    u32 m_cache_blocks = 0;

    // All public operations lock the mutex and release the GIL.
    std::mutex m_mutex;

    // Accessed while the mutex is locked.
    bool m_open = false;
    std::unique_ptr<prequel::file> m_database_file;
    std::unique_ptr<prequel::file> m_journal_file;
    std::unique_ptr<prequel::transaction_engine> m_engine;
};

/*
 * A checkpoint operation is automatically executed when the journal
 * has grown to this many or more bytes.
 */
static constexpr u64 journal_checkpoint_threshold = 1 << 20;

database::database(const std::string& path, u32 cache_blocks)
    : m_database_path(path)
    , m_journal_path(path + "-journal")
    , m_cache_blocks(cache_blocks) {
    open();
}

database::~database() {}

// Called from constructor only.
// Opens files, initializes the engine and accesses (or creates) the master block.
void database::open() {
    auto& vfs = prequel::system_vfs();
    m_database_file = vfs.open(m_database_path.c_str(), vfs.read_write, vfs.open_create);
    m_journal_file = vfs.open(m_journal_path.c_str(), vfs.read_write, vfs.open_create);
    m_engine = std::make_unique<prequel::transaction_engine>(*m_database_file, *m_journal_file,
                                                             4096, m_cache_blocks);

    static_assert(prequel::serialized_offset<&master_block::header>() == 0,
                  "Header must be at the beginning of the master block.");
    if (m_engine->size() == 0) {
        init_master_block();
    } else {
        check_master_block();
    }
    m_open = true;
}

void database::finish() {
    exec([&] {
        if (!m_open) {
            throw std::logic_error("database::finish() was already called.");
        }

        m_open = false;

        if (m_engine->journal_has_changes()) {
            m_engine->checkpoint();
        }
        m_engine.reset();
        m_journal_file.reset();
        m_database_file.reset();

        // It is safe to remove the journal file after a successful checkpoint.
        prequel::system_vfs().remove(m_journal_path.c_str());
    });
}

std::string database::dump() {
    std::ostringstream ss;

    exec_transaction([&](storage& store) {
        auto& alloc = dynamic_cast<prequel::default_allocator&>(store.get_allocator());
        fmt::print(ss, "Allocator state:\n");
        alloc.dump(ss);
        fmt::print(ss, "\n\n");

        store.dump(ss);
    });

    return ss.str();
}

u64 database::create_post(const std::string& user, const std::string& title,
                          const std::string& content) {
    u64 id = 0;
    exec_transaction([&](storage& store) { id = store.create_post(user, title, content); });
    return id;
}

bool database::create_comment(u64 post_id, const std::string& user, const std::string& content) {
    try {
        exec_transaction([&](storage& store) { store.create_comment(post_id, user, content); });
        return true;
    } catch (const not_found_error& e) {
        return false;
    }
}

py::list database::fetch_frontpage(size_t max_posts) {
    frontpage_result result;
    exec_transaction([&](const storage& store) { result = store.fetch_frontpage(max_posts); });

    py::list entries;
    for (const frontpage_result::post_entry& native_post : result.entries) {
        py::dict post;
        post["id"] = native_post.id;
        post["created_at"] = native_post.created_at;
        post["user"] = native_post.user;
        post["title"] = native_post.title;

        entries.append(std::move(post));
    }
    return entries;
}

py::object database::fetch_post(u64 post_id, size_t max_comments) {
    post_result result;
    try {
        exec_transaction(
            [&](const storage& store) { result = store.fetch_post(post_id, max_comments); });
    } catch (const not_found_error& e) {
        return py::none();
    }

    py::dict post;
    post["id"] = result.id;
    post["created_at"] = result.created_at;
    post["user"] = result.user;
    post["title"] = result.title;
    post["content"] = result.content;

    py::list comments;
    for (const post_result::comment_entry& native_comment : result.comments) {
        py::dict comment;
        comment["created_at"] = native_comment.created_at;
        comment["user"] = native_comment.user;
        comment["content"] = native_comment.content;
        comments.append(std::move(comment));
    }
    post["comments"] = std::move(comments);
    return post;
}

void database::init_master_block() {
    assert(m_engine->size() == 0);

    master_block master;
    master.header.magic = prequel::magic_header(FILE_FORMAT_MAGIC);
    master.header.version = FILE_FORMAT_VERSION;

    m_engine->begin();
    {
        m_engine->grow(1);

        auto handle = m_engine->overwrite_zero(prequel::block_index(0));
        handle.set(0, master);
    }
    m_engine->commit();
    m_engine->checkpoint();
}

void database::check_master_block() {
    assert(m_engine->size() > 0);

    m_engine->begin();
    {
        auto handle = m_engine->read(prequel::block_index(0));

        // Check magic header before reinterpreting the block in the application later on.
        file_header header = handle.get<file_header>(0);
        check_header(header);
    }
    m_engine->commit();
}

void database::check_header(const file_header& header) {
    if (header.magic != prequel::magic_header(FILE_FORMAT_MAGIC)) {
        throw std::runtime_error("Invalid file (wrong magic header).");
    }
    if (header.version != FILE_FORMAT_VERSION) {
        throw std::runtime_error(
            fmt::format("Unsupported version: File version is {} but only version {} is supported.",
                        header.version, FILE_FORMAT_VERSION));
    }
}

template<typename Func>
void database::exec(Func&& fn) {
    // Lock our own mutex and unlock the GIL for the duration of the function.
    // Must not execute python code within `fn`.
    py::gil_scoped_release release;
    std::unique_lock locked(m_mutex);
    fn();
}

/*
 * Begin a transaction and setup the required datastructure handles (allocator and storage).
 * Call commit() at the end, or rollback() if an exception has been thrown.
 */
template<typename Func>
void database::exec_transaction(Func&& fn) {
    exec([&] {
        if (!m_open) {
            throw std::logic_error("Transactions cannot be started after the database has been shut down.");
        }

        m_engine->begin();
        try {
            /*
             * TODO: Currently, all block references must be released
             * before either commit() or rollback() can be called.
             * This is why there are multiple scopes in this function; they
             * ensure that objects are destroyed before we act on the state
             * that was altered by those objects.
             * I'm thinking about relaxing this restriction, but i am not sure
             * about the safety aspects of it.
             */
            {
                auto first_block = m_engine->read(prequel::block_index(0));

                master_block master = first_block.get<master_block>(0);
                prequel::anchor_flag master_changed;
                prequel::anchor_handle anchor(master, master_changed);

                {
                    prequel::default_allocator alloc(anchor.template member<&master_block::alloc>(),
                                                     *m_engine);
                    storage store(anchor.template member<&master_block::store>(), alloc);
                    fn(store);
                }

                if (master_changed) {
                    first_block.set(0, master);
                }
            }

            m_engine->commit();
        } catch (...) {
            m_engine->rollback();
            throw;
        }

        if (m_engine->journal_size() > journal_checkpoint_threshold)
            m_engine->checkpoint();
    });
}

} // namespace blabber

using namespace blabber;

PYBIND11_MODULE(blabber_database, m) {
    m.doc() =
        "Blabber database native module.\n"
        "Implements database operations as atomic transactions "
        "using the prequel library.";

    py::class_<database>(m, "Database")
        .def(py::init<const std::string&, u32>(),
             "Create a new database object with the given path and cache size (in blocks).\n"
             "Database files must not be opened more than once.",
             py::arg("path"), py::arg("cache_blocks"))

        .def("create_post", &database::create_post, "Create a post.", py::arg("user"),
             py::arg("title"), py::arg("content"))

        .def("create_comment", &database::create_comment, "Create a comment in a post.",
             py::arg("post_id"), py::arg("user"), py::arg("content"))

        .def("fetch_frontpage", &database::fetch_frontpage,
             "Fetch the content of the front page. Returns the N latest posts.",
             py::arg("max_posts"))

        .def("fetch_post", &database::fetch_post,
             "Fetch the content of a post. Returns the N latest comments.", py::arg("post_id"),
             py::arg("max_comments"))

        .def("finish", &database::finish, "Perform a clean shutdown of the database.");
}
