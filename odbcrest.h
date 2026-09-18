/** ODBCREST */
#pragma once

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>
#include <odbcinst.h>

/** odbc4github  version */
#define ODBCREST_VERSION_MAJOR 1
#define ODBCREST_VERSION_MINOR 0

/** exports */
#if defined(_WIN32) || defined(__CYGWIN__)
    #define ODBCREST_API __declspec(dllexport)
#else
    #define ODBCREST_API
#endif

/** logs */
#if defined(ODBCREST_LOG)
    #include <stdio.h>
    #include <time.h>
    #define __TO_STR_RAW(a) #a
    #define __TO_STR(a) __TO_STR_RAW(a)
    #define ODBCREST_PRINT(format, ...)                         \
    {                                                           \
        FILE * fp = fopen(__TO_STR(ODBCREST_LOG), "a+");        \
        if ( fp )                                               \
        {                                                       \
            time_t tmNow;                                       \
            struct tm * pt;                                     \
            time(&tmNow);                                       \
            pt = localtime(&tmNow);                             \
            fprintf(fp, "odbcrest [ %02d:%02d:%02d ] ",         \
                    pt->tm_hour, pt->tm_min, pt->tm_sec);       \
            fprintf( fp, format, ##__VA_ARGS__);                \
            fprintf( fp, "\n" );                                \
            fclose( fp );                                       \
        }                                                       \
    }
#else
    #define ODBCREST_PRINT(format, ...)
#endif






