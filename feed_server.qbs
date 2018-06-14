import qbs 1.0
import qbs.Process
import "BaseDefines.qbs" as App


App{
    name: "feed_server"
    consoleApplication:false
    type:"application"
    qbsSearchPaths: sourceDirectory + "/modules"
    Depends { name: "Qt.core"}
    Depends { name: "Qt.sql" }
    Depends { name: "Qt.core" }
    Depends { name: "Qt.network" }
    Depends { name: "Qt.gui" }
    Depends { name: "Qt.concurrent" }
    Depends { name: "cpp" }
    Depends { name: "logger" }

    cpp.defines: base.concat(["L_LOGGER_LIBRARY"])
    cpp.includePaths: [
        sourceDirectory,
        sourceDirectory + "/include",
        sourceDirectory + "/libs",
        sourceDirectory + "/third_party/zlib",
        sourceDirectory + "/libs/Logger/include",
    ]

    files: [
        "include/Interfaces/base.h",
        "include/Interfaces/genres.h",
        "include/Interfaces/db_interface.h",
        "include/Interfaces/interface_sqlite.h",
        "include/Interfaces/recommendation_lists.h",
        "src/Interfaces/authors.cpp",
        "src/Interfaces/base.cpp",
        "src/Interfaces/genres.cpp",
        "src/Interfaces/db_interface.cpp",
        "src/Interfaces/interface_sqlite.cpp",
        "src/Interfaces/recommendation_lists.cpp",
        "include/container_utils.h",
        "include/generic_utils.h",
        "include/timeutils.h",
        "src/generic_utils.cpp",
        "include/querybuilder.h",
        "include/queryinterfaces.h",
        "include/core/section.h",
        "include/storyfilter.h",
        "include/url_utils.h",
        "third_party/sqlite/sqlite3.c",
        "third_party/sqlite/sqlite3.h",
        "src/sqlcontext.cpp",
        "src/main_feed_server.cpp",
        "src/pure_sql.cpp",
        "src/querybuilder.cpp",
        "src/regex_utils.cpp",
        "src/core/section.cpp",
        "src/sqlitefunctions.cpp",
        "src/storyfilter.cpp",
        "src/url_utils.cpp",
        "src/feeder_environment.cpp",
        "include/feeder_environment.h",
        "src/transaction.cpp",
        "include/transaction.h",
        "src/pagetask.cpp",
        "include/pagetask.h",
    ]

    cpp.staticLibraries: ["logger", "zlib", "quazip"]
}
