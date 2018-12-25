#ifndef BLABBER_STORAGE_HPP
#define BLABBER_STORAGE_HPP

#include "common.hpp"
#include "fixed_string.hpp"

#include <prequel/anchor_handle.hpp>
#include <prequel/container/btree.hpp>
#include <prequel/container/heap.hpp>
#include <prequel/container/list.hpp>
#include <prequel/serialization.hpp>

#include <string>
#include <variant>
#include <vector>

namespace blabber {

class database_error : public std::runtime_error {
public:
    using runtime_error::runtime_error;
};

class not_found_error : public database_error {
public:
    using database_error::database_error;
};

struct comment;
struct post;

// A string is either inlined (i.e. stored directly), if its size is small enough,
// or moved to the heap storage otherwise.
template<u32 Capacity>
using optimized_string = std::variant<fixed_cstring<Capacity>, prequel::heap::reference>;

/*
 * The format of posts stored on disk.
 */
struct post {
    // Unique id.
    u64 id = 0;

    // Unix timestamp (seconds, UTC).
    u64 created_at = 0;

    // User name (string).
    optimized_string<15> user;

    // User defined title (string).
    optimized_string<31> title;

    // User defined content (string).
    prequel::heap_reference content;

    // All comments in the order they have been inserted in (not indexed by anything).
    prequel::list<comment>::anchor comments;

    // Defines the binary layout. Must list all members once.
    static constexpr auto get_binary_format() {
        return prequel::binary_format(&post::id, &post::created_at, &post::user, &post::title,
                                      &post::content, &post::comments);
    }
};

/*
 * The format of comments stored on disk.
 */
struct comment {
    // Unix timestamp (seconds, UTC).
    u64 created_at = 0;

    // User name (string).
    optimized_string<15> user;

    // User defined content (string).
    prequel::heap_reference content;

    // Defines the binary layout.
    static constexpr auto get_binary_format() {
        return prequel::binary_format(&comment::created_at, &comment::user, &comment::content);
    }
};

struct frontpage_result {
    // The part of a post displayed on the front page.
    struct post_entry {
        u64 id = 0;
        u64 created_at = 0;
        std::string user;
        std::string title;
    };

    // Newest entry first.
    std::vector<post_entry> entries;
};

struct post_result {
    struct comment_entry {
        u64 created_at = 0;
        std::string user;
        std::string content;
    };

    u64 id = 0;
    u64 created_at = 0;
    std::string user;
    std::string title;
    std::string content;

    // Newest comment first.
    std::vector<comment_entry> comments;
};

class storage {
    /*
     * Stores posts and indexes them by their id.
     */
    using post_tree = prequel::btree<post, prequel::indexed_by_member<&post::id>>;

public:
    /*
     * The anchor of this class stores the fields that must be serialized in order to re-open the database.
     * Members are private (with the database being a friend for access) so that user code cannot accidentally
     * overwrite our persistent state.
     *
     * The user code is responsible for saving the anchor value when it has changed (e.g. at the end of a commit operation)
     * and loading it again when the database should be re-opened (for example, after a program restart).
     */
    class anchor {
        // IDs are simply incremented whenever a new post is created. 64-bit space is *very* unlikely to be exhausted :).
        u64 next_post_id = 1;

        // Anchor of the tree that manages all posts.
        post_tree::anchor posts;

        // Long strings are stored on the heap.
        prequel::heap::anchor strings;

        static constexpr auto get_binary_format() {
            return prequel::binary_format(&anchor::next_post_id, &anchor::posts, &anchor::strings);
        }

        friend storage;
        friend prequel::binary_format_access;
    };

public:
    explicit storage(prequel::anchor_handle<anchor> anchor_, prequel::allocator& alloc_);

    prequel::engine& get_engine() const { return m_alloc->get_engine(); }
    prequel::allocator& get_allocator() const { return *m_alloc; }

    u64 create_post(const std::string& user, const std::string& title, const std::string& content);

    void create_comment(u64 post_id, const std::string& user, const std::string& content);

    frontpage_result fetch_frontpage(size_t max_posts) const;

    post_result fetch_post(u64 post_id, size_t max_comments) const;

    void dump(std::ostream& os);

private:
    prequel::anchor_handle<anchor> m_anchor;
    prequel::allocator* m_alloc = nullptr;
    post_tree m_posts;
    prequel::heap m_strings;
};

} // namespace blabber

#endif // BLABBER_STORAGE_HPP
