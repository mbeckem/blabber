#include "database.hpp"

#include <pybind11/pybind11.h>

#include <mutex>

namespace py = pybind11;

namespace {

using namespace blabber;

// This class is exported to the python client code.
class py_database {
public:
    py_database(const std::string& path, u32 cache_blocks)
        : m_db(path, cache_blocks) {}

private:
    // Lock our own mutex and unlock the GIL for the duration of the function.
    // Must not execute python code within `fn`.
    template<typename Func>
    decltype(auto) exec(Func&& fn) const {
        py::gil_scoped_release release;
        std::unique_lock locked(m_mutex);
        return fn();
    }

public:
    py::int_ create_post(const std::string& user, const std::string& title, const std::string& content) {
        return exec([&]{
            return m_db.create_post(user, title, content);
        });
    }

    void create_comment(u64 post_id, const std::string& user, const std::string& content) {
        return exec([&]{
            return m_db.create_comment(post_id, user, content);
        });
    }

    py::list fetch_frontpage(size_t max_posts) {
        frontpage_result native = exec([&]{
            return m_db.fetch_frontpage(max_posts);
        });

        py::list entries;
        for (const frontpage_result::post_entry& native_post : native.entries) {
            py::dict post;
            post["id"] = native_post.id;
            post["created_at"] = native_post.created_at;
            post["user"] = native_post.user;
            post["title"] = native_post .title;

            entries.append(std::move(post));
        }
        return entries;
    }

    py::dict fetch_post(u64 post_id, size_t max_comments) {
        post_result native = exec([&]{
            return m_db.fetch_post(post_id, max_comments);
        });

        py::dict post;
        post["id"] = native.id;
        post["created_at"] = native.created_at;
        post["user"] = native.user;
        post["title"] = native.title;
        post["content"] = native.content;

        py::list comments;
        for (const post_result::comment_entry& native_comment : native.comments) {
            py::dict comment;
            comment["created_at"] = native_comment.created_at;
            comment["user"] = native_comment.user;
            comment["content"] = native_comment.content;
            comments.append(std::move(comment));
        }
        post["comments"] = std::move(comments);
        return post;
    }

    void finish() {
        exec([&]{
            m_db.finish();
        });
    }

private:
    // Prequel is single threaded right now. This mutex prevents
    // the python code from accidentally calling us from multiple threads
    // at once. Note that we still release the GIL so that other python code
    // can run concurrently.
    mutable std::mutex m_mutex;
    database m_db;
};

} // namespace

PYBIND11_MODULE(blabber_database, m) {
    m.doc() = "Blabber database native module.\n"
              "Implements database operations as atomic transactions "
              "using the prequel library.";

    py::class_<py_database>(m, "Database")
            .def(py::init<const std::string&, u32>(),
                 "Create a new database object with the given path and cache size (in blocks).\n"
                 "Database files must not be opened more than once.",
                 py::arg("path"), py::arg("cache_blocks"))

            .def("create_post", &py_database::create_post,
                 "Create a post.", py::arg("user"), py::arg("title"), py::arg("content"))

            .def("create_comment", &py_database::create_comment,
                 "Create a comment in a post.", py::arg("post_id"), py::arg("user"), py::arg("content"))

            .def("fetch_frontpage", &py_database::fetch_frontpage,
                 "Fetch the content of the front page. Returns the N latest posts.",
                 py::arg("max_posts"))

            .def("fetch_post", &py_database::fetch_post,
                 "Fetch the content of a post. Returns the N latest comments.",
                 py::arg("post_id"), py::arg("max_comments"))

            .def("finish", &py_database::finish,
                 "Perform a clean shutdown of the database.");
}

int main() {}
