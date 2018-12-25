#ifndef BLABBER_DATABASE_HPP
#define BLABBER_DATABASE_HPP

#include "common.hpp"
#include "storage.hpp"

#include <prequel/container/default_allocator.hpp>
#include <prequel/simple_file_format.hpp> // just for magic_header... FIXME move it
#include <prequel/transaction_engine.hpp>
#include <prequel/vfs.hpp>

#include <memory>

namespace blabber {

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
    explicit database(const std::string& path, u32 cache_blocks);
    ~database();

    database(const database&) = delete;
    database& operator=(const database&) = delete;

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

} // namespace blabber

#endif // BLABBER_DATABASE_HPP
