#include "storage.hpp"

#include <prequel/container/default_allocator.hpp>
#include <prequel/simple_file_format.hpp> // just for magic_header... FIXME move it
#include <prequel/transaction_engine.hpp>
#include <prequel/vfs.hpp>

#include <fmt/format.h>

#include <pybind11/pybind11.h>

#include <memory>

namespace blabber {

/*
 * A checkpoint operation is automatically executed when the journal
 * has grown to this many or more bytes.
 */
inline constexpr u64 journal_checkpoint_threshold = 1 << 20;

/*
 * The database is the top level interface exposed to clients (i.e. the python code).
 * All public member functions run in the context of a transaction and a therefore atomic.
 */
class database {
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

public:
    explicit database(const std::string& path, u32 cache_blocks)
        : m_database_path(path)
        , m_journal_path(path + "-journal")
        , m_cache_blocks(cache_blocks) {
        open();
    }

    // Todo: flag for disabling "sync on commit"

    void dump(std::ostream& os);

    u64 create_post(const std::string& user, const std::string& title, const std::string& content);
    void create_comment(u64 post_id, const std::string& user, const std::string& content);
    frontpage_result fetch_frontpage(size_t max_posts);
    post_result fetch_post(u64 post_id, size_t max_comments);

    // Called on a clean shutdown: performs a checkpoint and erases the journal.
    void finish();

private:
    void open();
    void init_master_block();
    void check_master_block();

    static void check_header(const file_header& header);

    template<typename Func>
    void run_in_transaction(Func&& fn);

private:
    std::string m_database_path;
    std::string m_journal_path;
    u32 m_cache_blocks = 0;

    bool m_open = false;
    std::unique_ptr<prequel::file> m_database_file;
    std::unique_ptr<prequel::file> m_journal_file;
    std::unique_ptr<prequel::transaction_engine> m_engine;
};

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
}

void database::dump(std::ostream& os) {
    run_in_transaction([&](storage& store) {
        auto& alloc = dynamic_cast<prequel::default_allocator&>(store.get_allocator());
        fmt::print(os, "Allocator state:\n");
        alloc.dump(os);
        fmt::print(os, "\n\n");

        store.dump(os);
    });
}

u64 database::create_post(const std::string& user, const std::string& title,
                          const std::string& content) {
    u64 id = 0;
    run_in_transaction([&](storage& store) { id = store.create_post(user, title, content); });
    return id;
}

void database::create_comment(u64 post_id, const std::string& user, const std::string& content) {
    run_in_transaction([&](storage& store) { store.create_comment(post_id, user, content); });
}

frontpage_result database::fetch_frontpage(size_t max_posts) {
    frontpage_result result;
    run_in_transaction([&](const storage& store) { result = store.fetch_frontpage(max_posts); });
    return result;
}

post_result database::fetch_post(u64 post_id, size_t max_comments) {
    post_result result;
    run_in_transaction(
        [&](const storage& store) { result = store.fetch_post(post_id, max_comments); });
    return result;
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

/*
 * Begin a transaction and setup the required datastructure handles (allocator and storage).
 * Call commit() at the end, or rollback() if an exception has been thrown.
 */
template<typename Func>
void database::run_in_transaction(Func&& fn) {
    if (!m_open) {
        throw std::logic_error("Transactions cannot be started after a clean shutdown.");
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
}

} // namespace blabber

int main() {
    using namespace blabber;

    const char* path = "blabber.db";
    const u32 cache_blocks = 4096;

    database db(path, cache_blocks);
    // db.dump(std::cout);

    u64 post_id = db.create_post("Test", "TestTitle", "Test Content");
    fmt::print("post created: {}\n", post_id);

    db.finish();
}
