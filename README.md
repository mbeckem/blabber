# Blabber

This is a demo project intended to show how to implement custom storage solutions using the [prequel library](https://github.com/mbeckem/prequel).

## Introduction

Blabber is a simplistic web application that allows users to submit posts and add comments to those posts.
The front page of Blabber shows only the most recent posts. Every post has its own page, where the most recent
comments are displayed.

*Please note that no effort whatsoever has been made to secure this application. Do not run it in a public environment.*

## Architecture

### HTTP Layer

Python is used to implement the web layer. The main libraries involved are asyncio, aiohttp (for I/O) and jinja2.
The application defines a few GET routes to view the front page and individual posts. Plain old HTML form submissions (via POST)
are used to create posts and comments. The implementation can be found in `app.py`.

### Database Layer

The Python application uses a native module programmed in C++ for its persistence. The code of that module can be found inside the
`database-plugin` directory. Compiling the database module yields a single dynamic object that can be imported by the Python interpreter.

The prequel library is used to implement the custom object storage for the web application. It uses three types of data structures:

1.  A single `btree` stores all posts, indexed by id. Post objects inside that btree store their id, a creation timestamp,
    and pointers to the user, title and content strings. Note that small strings are inlined into the post objects storage
    to reduce needless disk seeking.

2.  A single `heap` (heap as in "unordered heap file", not as in "priority queue") stores longer strings. These strings
    are being pointed to by the comment and post objects.

3.  Every post has a `list` instance to store the comments of that posts. Comments are inserted at the end of the list
    and are not indexed. The list is a simple data structure: a chain of blocks linked together by pointers, each block
    containing comment objects. Comments to not have an id, they only store a user name, content and a creation timestamp.

    In a real application, comments would likely be implemented with their own ids and with a single, large btree for all
    comments (like an SQL table with foreign keys to match them to their posts), but this project uses a linked list to showcase
    nested data structures.

The database back end supports transactions (atomic and durable) via the `transaction_engine`. Transactions are implemented
using a write ahead journal file placed next to the database file on disk. The content of the journal file is merged back to the
database in "checkpoint" operations when the journal becomes too large (> 1 MB right now) or on (clean) application shutdown.
As of right now, prequel does not support multiple concurrent threads, so the database plugin serializes all incoming transactions.

The classes responsible for the storage system can be found in `src/storage.hpp` and `src/storage.cpp`. The source code `src/database.cpp`
is responsible for opening files, starting and ending database transactions and exposing the interface to Python.


## Building

### Python

You need a recent version of Python (3.6+) in order to build and run this project.
Consider using a [virtual Python environment](https://docs.python.org/3.6/tutorial/venv.html)
for this project.

### Required system dependencies

You must compile the native database module before you can launch the python application.

- A recent Python interpreter (3.6+) and Python header files
- Boost header files (Note: boost will probably be eliminated as a dependency in the future)

On recent versions of Debian or Ubuntu, run as root:

```
    # apt-get install python3-dev libboost-all-dev
```

### Compilation steps

-   Fetch all submodules:

    ```
    $ git submodule update --init --recursive
    ```

-   Build the database plugin:

    ```
    $ cd database-plugin && mkdir build && cd build
    $ cmake ..
    $ make -j5
    ```

-   Copy the native module from the `build/src` directory
    next to `app.py` in the main directory.
    Note that the concrete name of the module depends on your platform,
    it should start with `blabber_database`.
    Example:

    ```
    $ cp src/blabber_database.cpython-36m-x86_64-linux-gnu.so ../..
    $ cd ../..
    ```

### Required Python libraries

The dependencies are listed in the file `requirements.txt`, you can install them using pip:

```
$ pip install -r requirements.txt
```

## Running

You can start the application by executing `app.py`. The builtin HTTP server will listen on port 8080:

```
$./app.py
======== Running on http://0.0.0.0:8080 ========
(Press CTRL+C to quit)
```
