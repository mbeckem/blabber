#!/usr/bin/env python3

import aiohttp
import asyncio
import concurrent
import logging

# This is our native database module
import blabber_database

from aiohttp import web

logger = logging.getLogger(__name__)

DATABASE_PATH = "./blabber.db"             # File path of our database file
DATABASE_CACHE_SIZE = (10 * 2**20) // 4096; # Memory cache size (unit is blocks of 4 KiB)

async def run_database(app):
    db = blabber_database.Database(DATABASE_PATH, DATABASE_CACHE_SIZE)
    app["db"] = db
    yield
    db.finish()

class BlabberHandlers:
    def __init__(self, app):
        self._loop = asyncio.get_event_loop()
        self._dbexec = concurrent.futures.ThreadPoolExecutor(max_workers = 1)
        self._dbpending = 0
    
    # Run all db operations in the single thread in order to queue them.
    # This will not block other I/O in the main event loop thread.
    # Note that the prequel library used for the database currently does not 
    # support multithreading.
    async def _dbop(self, op):
        if self._dbpending > 1000:
            raise RuntimeError("Too many pending db queries.")
        
        self._dbpending += 1
        try:
            return await self._loop.run_in_executor(self._dbexec, op)
        finally:
            self._dbpending -= 1
    
    async def index(self, request):
        db = request.app["db"]
        frontpage = await self._dbop(lambda: db.fetch_frontpage(max_posts = 100))
        return web.json_response(frontpage)
    
    async def show_post(self, request):
        db = request.app["db"]
        post_id = int(request.match_info["post_id"])
        post = await self._dbop(lambda: db.fetch_post(post_id = post_id, max_comments = 100))
        
        if post is None:
            raise web.HTTPNotFound()
        return web.json_response(post)
    
    async def submit_post(self, request):
        db = request.app["db"]
        data = await request.post()
        user = data["user"]
        title = data["title"]
        content = data["content"]
        
        post_id = await self._dbop(lambda: db.create_post(user = user, title = title, content = content))
        location = request.app.router["show_post"].url_for(post_id = str(post_id))
        raise web.HTTPFound(location)
    
    async def submit_comment(self, request):
        db = request.app["db"]
        data = await request.post()
        post_id = int(request.match_info["post_id"])
        user = data["user"]
        content = data["content"]
        
        ok = await self._dbop(lambda: db.create_comment(post_id = post_id, user = user, content = content))
        if not ok:
            raise web.HTTPNotFound()
        location = request.app.router["show_post"].url_for(post_id = str(post_id))
        raise web.HTTPFound(location)


def main():
    app = web.Application()
    app.cleanup_ctx.append(run_database)

    blabber = BlabberHandlers(app)
    
    app.add_routes([
        web.get("/", blabber.index, name = "index"),
        web.post("/post", blabber.submit_post, name = "submit_post"),
        web.get("/post/{post_id:\d+}", blabber.show_post, name = "show_post"),
        web.post("/post/{post_id:\d+}/comment", blabber.submit_comment, name = "submit_comment"),
    ])
    app.router.add_static("/static", path = "./static", name = "static")
    web.run_app(app)


if __name__ == "__main__":
    main()
