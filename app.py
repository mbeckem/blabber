#!/usr/bin/env python3

import aiohttp
import asyncio
import concurrent
import datetime
import jinja2
import logging
import os

# This is our native database module
import blabber_database

from aiohttp import web

logger = logging.getLogger(__name__)

DEV = True
ROOT_DIRECTORY = os.path.dirname(os.path.realpath(__file__))

DATABASE_PATH = "./blabber.db"             # File path of our database file
DATABASE_CACHE_SIZE = (10 * 2**20) // 4096; # Memory cache size (unit is blocks of 4 KiB)


# Called from html templates
def format_datetime(timestamp):
    dt = datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc)
    local = dt.astimezone(tz = None)
    return str(local) # TODO

def newline_to_br(value):
    result = str(jinja2.escape(value)).replace("\n", "<br>\n");
    return jinja2.Markup(result)


class BlabberHandlers:
    def __init__(self, app):
        self._app = app
        self._loop = asyncio.get_event_loop()

        # Jinja settings
        self._jinjaenv = jinja2.Environment(
            autoescape = True,
            trim_blocks = True,
            lstrip_blocks = True,
            auto_reload = True if DEV else False,
            loader = jinja2.FileSystemLoader(os.path.join(ROOT_DIRECTORY, "templates")))
        self._jinjaenv.filters["datetime"] = format_datetime
        self._jinjaenv.filters["newline_to_br"] = newline_to_br
        self._jinjaenv.globals.update(index_location = self._index_location,
                                      post_location = self._post_location,
                                      submit_post_location = self._submit_post_location,
                                      submit_comment_location = self._submit_comment_location)

        # Database state
        self._dbexec = concurrent.futures.ThreadPoolExecutor(max_workers = 1)
        self._dbpending = 0

    # Run the database operations in a worker thread so we don't block other network I/O.
    # The database does not support multithreading right now, so more than one worker would be useless.
    # We only allow a maximum number of pending operations in case our database is too slow
    # to handle the incoming requests (they would queue up without bounds otherwise).
    async def _dbop(self, op):
        if self._dbpending > 1000:
            raise RuntimeError("Too many pending db queries.")

        self._dbpending += 1
        try:
            return await self._loop.run_in_executor(self._dbexec, op)
        finally:
            self._dbpending -= 1

    def _render_html(self, tmpl_name, **context):
        template = self._jinjaenv.get_template(tmpl_name)
        html = template.render(**context)
        return web.Response(
            status = 200,
            content_type = "text/html",
            text = html
        )

    # Returns path to the index page.
    def _index_location(self):
        return self._app.router["index"].url_for()

    # Returns the path to the page for the post with the given id.
    def _post_location(self, post_id):
        return self._app.router["show_post"].url_for(post_id = str(post_id))

    # Returns the path to the submit-post endpoint.
    def _submit_post_location(self):
        return self._app.router["submit_post"].url_for()

    # Returns the path to the submit-comment endpoint.
    def _submit_comment_location(self, post_id):
        return self._app.router["submit_comment"].url_for(post_id = str(post_id))

    async def _show_index_impl(self, request, existing_form):
        db = request.app["db"]
        frontpage = await self._dbop(lambda: db.fetch_frontpage(max_posts = 100))
        form = dict()
        return self._render_html("index.html", frontpage = frontpage, form = existing_form)

    async def _show_post_impl(self, request, post_id, existing_form):
        db = request.app["db"]
        post = await self._dbop(lambda: db.fetch_post(post_id = post_id, max_comments = 100))

        if post is None:
            raise web.HTTPNotFound()
        return self._render_html("post.html", post = post, form = existing_form)


    async def index(self, request):
        return await self._show_index_impl(request, None)

    async def show_post(self, request):
        post_id = int(request.match_info["post_id"])
        return await self._show_post_impl(request, post_id, None)

    async def submit_post(self, request):
        db = request.app["db"]
        data = await request.post()

        form_errors = []

        user = data["user"].strip()
        if user == "":
            form_errors.append("Please enter a non-empty user name.")

        title = data["title"].strip()
        if title == "":
            form_errors.append("Please enter a non-empty post title.")

        content = data["content"].strip()
        if content == "":
            form_errors.append("Please enter a non-empty post content.")

        if form_errors:
            form = {
                "errors": form_errors,
                "user": user,
                "title": title,
                "content": content
            }
            return await self._show_index_impl(request, form)

        post_id = await self._dbop(lambda: db.create_post(user = user, title = title, content = content))
        raise web.HTTPFound(self._post_location(post_id))

    async def submit_comment(self, request):
        db = request.app["db"]
        data = await request.post()

        form_errors = []
        post_id = int(request.match_info["post_id"])

        user = data["user"].strip()
        if user == "":
            form_errors.append("Please enter a non-empty user name.")

        content = data["content"].strip()
        if content == "":
            form_errors.append("Please enter a non-empty comment.")

        if form_errors:
            form = {
                "errors": form_errors,
                "user": user,
                "content": content,
            }
            return await self._show_post_impl(request, post_id, form)

        ok = await self._dbop(lambda: db.create_comment(post_id = post_id, user = user, content = content))
        if not ok:
            raise web.HTTPNotFound()
        raise web.HTTPFound(self._post_location(post_id))

    async def dump(self, request):
        db = request.app["db"]
        data = await self._dbop(lambda: db.dump())
        return web.Response(text = data)


def main():

    async def run_database(app):
        app["db"] = blabber_database.Database(DATABASE_PATH, DATABASE_CACHE_SIZE)
        yield
        app["db"].finish()

    app = web.Application()
    app.cleanup_ctx.append(run_database)

    blabber = BlabberHandlers(app)

    app.add_routes([
        web.get("/", blabber.index, name = "index"),
        web.post("/post", blabber.submit_post, name = "submit_post"),
        web.get("/post/{post_id:\d+}", blabber.show_post, name = "show_post"),
        web.post("/post/{post_id:\d+}/comment", blabber.submit_comment, name = "submit_comment"),
        web.get("/dump", blabber.dump, name = "dump"),
    ])
    app.router.add_static("/static", path = os.path.join(ROOT_DIRECTORY, "static"), name = "static")
    web.run_app(app)


if __name__ == "__main__":
    main()
