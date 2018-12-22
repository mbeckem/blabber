#include "database.hpp"

#include <algorithm>
#include <ctime>

namespace blabber {

// Dereference the string and load it from the heap.
static std::string load_string(const prequel::heap& h, const prequel::heap_reference& ref) {
    std::string loaded;
    loaded.resize(h.size(ref));
    h.load(ref, reinterpret_cast<byte*>(&loaded[0]), loaded.size());
    return loaded;
}

// Stores the string on the heap and returns a reference to its location.
static prequel::heap_reference store_string(prequel::heap& h, const std::string& str) {
    return h.allocate(reinterpret_cast<const byte*>(str.data()), str.size());
}

// Loads the string, dereferencing if necessary.
template<u32 Capacity>
static std::string
load_optimized_string(const prequel::heap& h, const optimized_string<Capacity>& str) {
    std::string loaded;

    struct visitor {
        const prequel::heap* h;

        // Called when the string was inlined. We can get the string content
        // from the object itself.
        std::string operator()(const fixed_cstring<Capacity>& str) const {
            return std::string(str.begin(), str.end());
        }

        // Called when the string is stored in the heap. We must dereference
        // the heap ref in order to load to string content.
        std::string operator()(const prequel::heap_reference& ref) const {
            return load_string(*h, ref);
        }
    };

    return std::visit(visitor{&h}, str);
}

// Stores the string by either inlining it (small strings) or saving it on the heap.
template<u32 Capacity>
static optimized_string<Capacity> store_optimized_string(prequel::heap& h, const std::string& str) {
    if (str.size() > std::numeric_limits<u32>::max()) {
        // Sanity check. Size of objects in prequel::heap is limited to 2^32 - 1 right now.
        throw database_error("String is too large.");
    }

    // Check whether the string fits into the available optimized storage.
    if (str.size() <= Capacity) {
        return fixed_cstring<Capacity>(str);
    }

    // Store long strings on the heap.
    return store_string(h, str);
}

static u64 current_timestamp() {
    /* should be UTC seconds on all relevant platforms */
    time_t t = std::time(0);
    if (t < 0)
        throw std::runtime_error("time() failed.");
    return static_cast<u64>(t);
}

database::database(prequel::anchor_handle<anchor> anchor_, prequel::allocator& alloc_)
    : m_anchor(std::move(anchor_))
    , m_alloc(&alloc_)
    , m_posts(m_anchor.member<&anchor::posts>(), alloc_)
    , m_strings(m_anchor.member<&anchor::strings>(), alloc_) {}

u64 database::create_post(const std::string& user, const std::string& title,
                         const std::string& content) {
    const u64 id = m_anchor.get<&anchor::next_post_id>();
    if (id == 0) { // id wrap around, practially impossible
        throw database_error("ID space exhausted.");
    }

    post new_post;
    new_post.id = id;
    new_post.created_at = current_timestamp();
    new_post.user = store_optimized_string<15>(m_strings, user);
    new_post.title = store_optimized_string<31>(m_strings, title);
    new_post.content = store_string(m_strings, content);
    m_posts.insert(new_post);

    m_anchor.set<&anchor::next_post_id>(id + 1);
    return id;
}

void database::create_comment(u64 post_id, const std::string& user, const std::string& content) {
    // First, find the post. Then insert the new comment into the list.
    auto post_cursor = m_posts.find(post_id);
    if (!post_cursor) {
        throw not_found_error("Post not found.");
    }

    post found_post = post_cursor.get();
    prequel::anchor_flag post_changed;

    // Open the list from the list anchor in the post structure.
    {
        prequel::list<comment> comments(prequel::anchor_handle(found_post.comments, post_changed),
                                        *m_alloc);

        // Create and insert the new comment.
        comment new_comment;
        new_comment.created_at = current_timestamp();
        new_comment.user = store_optimized_string<15>(m_strings, user);
        new_comment.content = store_string(m_strings, content);
        comments.push_back(new_comment);
    }

    // The list anchor has changed because of the insertion, we MUST update the post entry.
    if (post_changed) {
        post_cursor.set(found_post);
    }
}

frontpage_result database::fetch_frontpage(size_t max_posts) const {
    std::vector<post> found_posts;
    {
        // Iterate from the end, in reverse order.
        auto tree_cursor = m_posts.create_cursor(m_posts.seek_max);
        while (tree_cursor && found_posts.size() < max_posts) {
            found_posts.push_back(tree_cursor.get());
            tree_cursor.move_prev();
        }
        std::reverse(found_posts.begin(), found_posts.end());
    }

    frontpage_result result;
    for (const post& p : found_posts) {
        frontpage_result::post_entry entry;
        entry.id = p.id;
        entry.created_at = p.created_at;
        entry.user = load_optimized_string(m_strings, p.user);
        entry.title = load_optimized_string(m_strings, p.title);

        result.entries.push_back(std::move(entry));
    }
    return result;
}

post_result database::fetch_post(u64 post_id, size_t max_comments) const {
    // First, find the post. Then insert the new comment into the list.
    auto post_cursor = m_posts.find(post_id);
    if (!post_cursor) {
        throw not_found_error("Post not found.");
    }

    post found_post = post_cursor.get();
    prequel::anchor_flag post_changed;
    std::vector<comment> found_comments;
    {

        // Open the list from the list anchor in the post structure.
        prequel::list<comment> comments(prequel::anchor_handle(found_post.comments, post_changed),
                                        *m_alloc);
        auto cursor = comments.create_cursor(comments.seek_last);
        while (cursor && found_comments.size() < max_comments) {
            found_comments.push_back(cursor.get());
            cursor.move_prev();
        }
        std::reverse(found_comments.begin(), found_comments.end());
    }

    if (post_changed) {
        // The list will not be modified by the operations above.
        throw std::logic_error("Must not modify the post in a read only operation.");
    }

    post_result result;
    result.id = found_post.id;
    result.created_at = found_post.created_at;
    result.user = load_optimized_string(m_strings, found_post.user);
    result.title = load_optimized_string(m_strings, found_post.title);
    result.content = load_string(m_strings, found_post.content);

    /*
     * Load the strings for the comments from the heap. Note that this would
     * involve lots of seeking in a real application, because we load the strings
     * in comment-order and not in the order in which they appear on disk.
     *
     * Sorting the string references before loading them (heap_reference supports a
     * sensible ordering) would improve the efficiency of this operation by a lot, but this
     * is currently not necessary: We don't support deletion, so all entries are somewhat
     * in order anyway.
     */
    for (const comment& c : found_comments) {
        post_result::comment_entry entry;
        entry.created_at = c.created_at;
        entry.user = load_optimized_string(m_strings, c.user);
        entry.content = load_string(m_strings, c.content);

        result.comments.push_back(std::move(entry));
    }
    return result;
}

} // namespace blabber
