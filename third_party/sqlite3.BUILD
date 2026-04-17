# sqlite3 3.47.2 — amalgamation build

cc_library(
    name = "sqlite3",
    srcs = ["sqlite3.c"],
    hdrs = [
        "sqlite3.h",
        "sqlite3ext.h",
    ],
    copts = select({
        "@platforms//os:windows": [
            "/DSQLITE_THREADSAFE=1",
            "/DSQLITE_ENABLE_FTS5",
            "/DSQLITE_ENABLE_JSON1",
            "/W0",
        ],
        "//conditions:default": [
            "-DSQLITE_THREADSAFE=1",
            "-DSQLITE_ENABLE_FTS5",
            "-DSQLITE_ENABLE_JSON1",
            "-w",
        ],
    }),
    visibility = ["//visibility:public"],
)
