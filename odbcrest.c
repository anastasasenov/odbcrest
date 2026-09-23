#include "odbcrest.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <threads.h>
#include <uchar.h>
#include <curl/curl.h>
#include <json-c/json.h>


/** ODBC structs ( hidden ) */

#define ODBCREST_BUFSIZ BUFSIZ

/** insternal statements */
#define ODBCREST_STMT_TABLES  (-1)
#define ODBCREST_STMT_COLUMNS (-2)

/** env-var for configuraion */
#define ODBCREST_CFG       "odbcrest.ini"
#define ODBCREST_ENV       "ODBCREST_INI"
#define ODBCREST_DEFAULT   ""
#define ODBCREST_TABLE     "table%d"
#define ODBCREST_NAME      "name"
#define ODBCREST_JSONID    "jsonid%d"
#define ODBCREST_JSONARRAY "jsonarray"
#define ODBCREST_URL       "url"
#define ODBCREST_HEADER    "header%d"

/** (char*) <-> (unsigned char*) */
#define TO_SQL(str) ((SQLCHAR*)(str))
#define TO_CHAR(sql_str) ((char*)(sql_str))

typedef struct {

    SQLUSMALLINT m_uColumnNumber;
    SQLSMALLINT m_nTargetType;
    SQLPOINTER m_pTargetValue;
    SQLLEN m_uBufferLength;
    SQLLEN *m_puStrLen_or_Ind;

} TBindCol;

typedef struct {

    TBindCol *m_pBinding;
    unsigned m_uNumOfBind;

} TBindRec;

typedef struct {

    SQLCHAR m_szColumnName[ODBCREST_BUFSIZ];
    SQLSMALLINT m_nDataType;
    SQLULEN m_uColumnSize;

} TDescCol;

typedef struct {

    TDescCol *m_pDescCol;
    unsigned m_uNumOfDesc;

} TDescRec;

typedef struct {

    SQLINTEGER m_odbc_version;

} TEnv;

typedef struct {

    TEnv* m_pEnv;
    bool m_bConnected;

} TDbc;

typedef struct {

    TDbc* m_pDbc;
    int m_nStmt;
    unsigned m_uRecNo;
    TBindRec* m_pBindRec;
    TDescRec* m_pDescRec;
    struct json_object * m_pJsonObj;
    struct json_object * m_pJsonRes;
    char m_szErrMsg[ODBCREST_BUFSIZ];

} TStmt;

typedef struct {

    TStmt* m_pStmt;

} TDesc;

typedef struct {
    
    char * m_pData;
    size_t m_nSize;

} TBuf;

/** globals */
static mtx_t g_mtxSo;

/** helpers */
static
void _init_curl() {

    if (mtx_lock(&g_mtxSo) == thrd_success) {

        curl_global_init( CURL_GLOBAL_DEFAULT );

        mtx_unlock( &g_mtxSo );
    }
}

static
void _deinit_curl() {

    if (mtx_lock(&g_mtxSo) == thrd_success) {

        curl_global_cleanup();

        mtx_unlock( &g_mtxSo );
    }
}

static
void __attribute__((constructor)) init_so(void) {

    mtx_init( &g_mtxSo, mtx_plain );

    _init_curl();
}

static
void __attribute__((destructor)) deinit_so(void) {
    
    _deinit_curl();

    mtx_destroy( &g_mtxSo );
}

static
const char* _get_ini(void) {
    
    const char * pIniFile = ODBCREST_CFG;
    const char * pEnv = getenv(ODBCREST_ENV);

    return ( pEnv ? pEnv : pIniFile );
}

static
int _get_ini_val(
    const char* pszSection,
    const char* pszKey,
    char* pszValue,
    unsigned uValueSize) {

    *pszValue = 0;
    SQLGetPrivateProfileString(
        pszSection,
        pszKey,
        ODBCREST_DEFAULT,
        pszValue,
        uValueSize,
        _get_ini()
    );
    
    return strlen(pszValue);
}

static
void* _ext_mem(void* pMem, size_t uSize, size_t uNewSize) {

    void *pRet = NULL;
    
    pRet = malloc( uNewSize );
    memcpy( pRet, pMem, uSize );
    free( pMem );
    
    return pRet;
}

static
size_t _curl_cb(
    void * pContent,
    size_t nSize,
    size_t nMemb,
    void * pUserParm ) {

    size_t nRealSize = nSize * nMemb;

    TBuf* pMem = (TBuf*) pUserParm;

    pMem->m_pData = _ext_mem( pMem->m_pData, pMem->m_nSize, pMem->m_nSize + nRealSize + 1);
    memcpy(&(pMem->m_pData[pMem->m_nSize]), pContent, nRealSize);
    pMem->m_nSize += nRealSize;
    pMem->m_pData[ pMem->m_nSize ] = 0; // nullterm

    return nRealSize;
}

static
void _init_BindRec(TBindRec * p) {
    
    if ( p ) {
        
        p->m_pBinding = NULL;
        p->m_uNumOfBind = 0;
    }
}

static
void _free_BindRec(TBindRec * p) {

    if ( p ) {
        
        if ( p->m_pBinding ) {

            free( p->m_pBinding );
        }
        
        _init_BindRec( p );
    }
}

static
void _init_DescRec(TDescRec * p) {
    
    if ( p ) {
        
        p->m_pDescCol = NULL;
        p->m_uNumOfDesc = 0;
    }
}

static
void _free_DescRec(TDescRec * p) {

    if ( p ) {
        
        if ( p->m_pDescCol ) {

            free( p->m_pDescCol );
        }
        
        _init_DescRec( p );
    }
}

static
void _init_Stmt(TStmt * p) {
    
    if ( p ) {
        
        p->m_pDbc = NULL;
        p->m_nStmt = 0;
        p->m_uRecNo = 0;
        p->m_pBindRec = NULL;
        p->m_pDescRec = NULL;
        p->m_pJsonObj = NULL;
        p->m_pJsonRes = NULL;
        p->m_szErrMsg[0] = 0;
    }
}

static
void _free_Stmt(TStmt * p) {

    if ( p ) {
        
        _free_BindRec( p->m_pBindRec );
        _free_DescRec( p->m_pDescRec );

        if ( p->m_pBindRec ) {
            free( p->m_pBindRec );
        }

        if ( p->m_pDescRec ) {
            free( p->m_pDescRec );
        }

        if ( p->m_pJsonObj ) {
            json_object_put( p->m_pJsonObj );
        }

        _init_Stmt( p );
    }
}

static
size_t _psz_convert(
    SQLCHAR* pUStr,
    const char* pszStr,
    SQLINTEGER nBufferLength,
    SQLINTEGER* pStringLengthPtr) {

    size_t nRet = 0;
    SQLINTEGER nStrLen = strlen(pszStr);

    if ( nStrLen < nBufferLength ) {

        strcpy( TO_CHAR(pUStr), pszStr );
        nRet = (1 + strlen(pszStr) );

    } else {

        nRet = nBufferLength;
        memcpy( pUStr, pszStr, nRet );
    }
    
    if ( pStringLengthPtr ) {

        *pStringLengthPtr = nRet;
    }
    
    return nRet;
}

static
size_t _ustr_to_psz(
    char* pszStr,
    SQLINTEGER nStrLength,
    SQLCHAR* pUStr,
    SQLINTEGER nUStrLength) {

    size_t nRet = 0;
    
    if ( SQL_NTS == nUStrLength ) {

        nUStrLength = 0;
        while ( pUStr[ nUStrLength ])
            nUStrLength ++;
    }

    if ( nStrLength > 0 ) {

        *pszStr = 0;
        if ( (nStrLength > nUStrLength ) && ( nUStrLength > 0 ) ) {

            for ( int i = 0; i < nUStrLength; i ++ ) {

                pszStr[ i ] = (char)( pUStr[ i ] );
                pszStr[ i + 1 ] = 0;
            }
        }
        
        nRet = 1 + strlen(pszStr);
    }
    
    return nRet;
}

static 
SQLRETURN _find_and_describe_tbl(TStmt * pStmt, SQLCHAR* psUstr, SQLINTEGER nLen) {
    
    SQLRETURN nRet = SQL_NO_DATA;
    char szStmt[ODBCREST_BUFSIZ];

    szStmt[ 0 ] = 0;
    _ustr_to_psz( szStmt, ODBCREST_BUFSIZ - 1, psUstr, nLen );
    if ( pStmt && (strlen( szStmt ) > 0) ) {
        
        int nPosTbl = 0;
        while ( true ) {

            char szSection[ODBCREST_BUFSIZ];
            char szName[ODBCREST_BUFSIZ];

            sprintf(szSection, ODBCREST_TABLE, ++nPosTbl );
            if ( ! _get_ini_val( szSection, ODBCREST_NAME, szName, sizeof(szName) - 1) ) {
                break;
            }

            /** select * from tbl - there no statement validation !!! */
            if ( strstr(szStmt, szName) != NULL ) {

                int nPosCol = 0;
                char szKey[ODBCREST_BUFSIZ];
                char szCol[ODBCREST_BUFSIZ];

                while ( true ) {
                    
                    TDescCol* pDescCol = NULL;

                    sprintf(szKey, ODBCREST_JSONID, ++nPosCol );
                    if ( ! _get_ini_val( szSection, szKey, szCol, sizeof(szCol) - 1) ) {
                        break;
                    }
                    
                    if ( pStmt->m_pDescRec &&
                         pStmt->m_pDescRec->m_pDescCol &&
                         ( pStmt->m_pDescRec->m_uNumOfDesc > 0 ) ) {

                        pStmt->m_pDescRec->m_uNumOfDesc ++;
                        pStmt->m_pDescRec->m_pDescCol = (TDescCol*)
                            _ext_mem(pStmt->m_pDescRec->m_pDescCol,
                                    sizeof(TDescCol) * ( pStmt->m_pDescRec->m_uNumOfDesc - 1 ),
                                    sizeof(TDescCol) * ( pStmt->m_pDescRec->m_uNumOfDesc ));

                        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ pStmt->m_pDescRec->m_uNumOfDesc - 1 ] );

                    } else {
                        
                        pStmt->m_pDescRec = (TDescRec*) malloc( sizeof( TDescRec ) );
                        pStmt->m_pDescRec->m_uNumOfDesc = 1;                      
                        pStmt->m_pDescRec->m_pDescCol = (TDescCol*) malloc( sizeof( TDescCol ) );
                        pDescCol = pStmt->m_pDescRec->m_pDescCol;
                    }
                    
                    strcpy( TO_CHAR(pDescCol->m_szColumnName), szCol);
                    pDescCol->m_nDataType = SQL_VARCHAR;
                    pDescCol->m_uColumnSize = 255;
                }

                if ( pStmt->m_pDescRec && ( pStmt->m_pDescRec->m_uNumOfDesc > 0 ) ) {

                    pStmt->m_nStmt = nPosTbl;
                    nRet = SQL_SUCCESS;
                }
            }
        }
    }
    
    return nRet;
}

static 
struct json_object* _fetch_json(TStmt * pStmt) {

    struct json_object* pRet = NULL;
    CURL* pHCurl = curl_easy_init();
    struct curl_slist * pHdr = NULL;
    char szSection[ODBCREST_BUFSIZ];
    char szUrl[ODBCREST_BUFSIZ];
    TBuf buf;

    buf.m_pData = NULL;
    buf.m_nSize = 0;

    sprintf( szSection, ODBCREST_TABLE, pStmt->m_nStmt );
    if ( _get_ini_val( szSection, ODBCREST_URL, szUrl, sizeof(szUrl) - 1) ) {

        int uPosHdr = 0;
        CURLcode res;
        char szHdr[ODBCREST_BUFSIZ];
        char szHdrValue[ODBCREST_BUFSIZ];
        char szArrayId[ODBCREST_BUFSIZ];

        curl_easy_setopt(pHCurl, CURLOPT_URL, szUrl);
        curl_easy_setopt(pHCurl, CURLOPT_WRITEFUNCTION, _curl_cb);
        curl_easy_setopt(pHCurl, CURLOPT_WRITEDATA, (void *)&buf);
        _get_ini_val( szSection, ODBCREST_JSONARRAY, szArrayId, sizeof(szArrayId) - 1);
        
        while ( true ) {
            
            sprintf(szHdr, ODBCREST_HEADER, ++uPosHdr );
            if ( ! _get_ini_val( szSection, szHdr, szHdrValue, sizeof(szHdrValue) - 1) ) {
                break;
            }
            
            pHdr = curl_slist_append(pHdr, szHdrValue);
        }
        
        if ( pHdr ) {

            curl_easy_setopt(pHCurl, CURLOPT_HTTPHEADER, pHdr);
        }
        
        res = curl_easy_perform(pHCurl);
        if ( pHCurl && ( CURLE_OK == res ) ) {

            pStmt->m_pJsonObj = json_tokener_parse( buf.m_pData ); /** to free */
            pRet = pStmt->m_pJsonObj;
            if ( pRet && strlen(szArrayId) ) {

                json_object_object_get_ex(pRet, szArrayId, &pRet);
            }
            pStmt->m_pJsonRes = pRet;
        }
        
        if ( pHCurl && ( CURLE_OK != res ) ) {

            strncpy( pStmt->m_szErrMsg, curl_easy_strerror(res), ODBCREST_BUFSIZ - 2 );
            pStmt->m_szErrMsg[ ODBCREST_BUFSIZ - 2 ] = 0;
        }
    }

    if ( pHCurl ) {

        curl_easy_cleanup(pHCurl);
    }
    
    if ( pHdr ) {
        
        curl_slist_free_all( pHdr );
    }
    
    if ( buf.m_pData ) {

        free( buf.m_pData );
    }

    return pRet;
}

static 
SQLRETURN _fetch_tbl(TStmt * pStmt, bool bInc) {
    
    SQLRETURN nRet = SQL_NO_DATA;
    struct json_object* pJsonObj = NULL;

    if ( pStmt->m_pJsonRes ) {

        pJsonObj = pStmt->m_pJsonRes;

    } else {

        pJsonObj = _fetch_json( pStmt );
    }

    if ( bInc ) {

        (pStmt->m_uRecNo) ++; /** 1st */
    }

    if ( pJsonObj ) {

        if (json_object_get_type( pJsonObj ) == json_type_array) {

            unsigned uIdx = (pStmt->m_uRecNo - 1);
            size_t nArrayLen = json_object_array_length( pJsonObj );

            if ( uIdx < nArrayLen ) {

                struct json_object * pRecObj =
                    json_object_array_get_idx(pJsonObj, uIdx);

                if ( pStmt->m_pBindRec && pStmt->m_pBindRec->m_pBinding ) {
                
                    for ( unsigned i = 0; i < pStmt->m_pBindRec->m_uNumOfBind; i ++ ) {

                        SQLLEN nLen = 0;
                        TBindCol * pBinding = &( pStmt->m_pBindRec->m_pBinding[i] );
                        TDescCol * pDescCol = NULL;

                        if ( pStmt->m_pDescRec && pStmt->m_pDescRec->m_pDescCol ) {
                            
                            if ( ( pBinding->m_uColumnNumber > 0 ) &&
                                 ( pBinding->m_uColumnNumber <= pStmt->m_pDescRec->m_uNumOfDesc ) ) {

                                pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ pBinding->m_uColumnNumber - 1 ] );
                            }
                        }
                        
                        if ( pBinding && pDescCol && pRecObj ) {
                            
                            char szName[ODBCREST_BUFSIZ];
                            struct json_object * pObj = NULL;

                            strcpy(szName, TO_CHAR(pDescCol->m_szColumnName));
                            json_object_object_get_ex(pRecObj, szName, &pObj);

                            if ( pObj ) {

                                nLen = (-1) + _psz_convert(
                                    pBinding->m_pTargetValue, 
                                    json_object_get_string(pObj), 
                                    pBinding->m_uBufferLength, 
                                    NULL);
                            }
                        }
                        
                        if ( pBinding->m_puStrLen_or_Ind ) {

                            *( pBinding->m_puStrLen_or_Ind ) = nLen;
                        }
                    }
                }

                nRet = SQL_SUCCESS;
            }
        }
    }

    return nRet;
}

static 
SQLRETURN _fetch_tables(TStmt * pStmt, bool bInc) {

    SQLRETURN nRet = SQL_NO_DATA;
    const int nTblNameCol = 3;
    const int nTblTypeCol = 4;
    const char* pszTblType = "TABLE";
    char szSection[ODBCREST_BUFSIZ];
    char szName[ODBCREST_BUFSIZ];

    if ( bInc ) {

        (pStmt->m_uRecNo) ++; /** 1st */
    }

    sprintf(szSection, ODBCREST_TABLE, (int)(pStmt->m_uRecNo));
    SQLLEN nLenTblType = strlen(pszTblType);
    SQLLEN nLenTblName = strlen(szName);
    if ( _get_ini_val( szSection, ODBCREST_NAME, szName, sizeof(szName) - 1) ) {

        if ( pStmt->m_pBindRec && pStmt->m_pBindRec->m_pBinding ) {
        
            for ( unsigned i = 0; i < pStmt->m_pBindRec->m_uNumOfBind; i ++ ) {

                SQLLEN nLen = 0;
                TBindCol * pBinding = &( pStmt->m_pBindRec->m_pBinding[i] );

                if ( nTblTypeCol == pBinding->m_uColumnNumber ) {
                    
                    _psz_convert(pBinding->m_pTargetValue, pszTblType, pBinding->m_uBufferLength, NULL);
                    nLen = nLenTblType;

                } else if ( nTblNameCol == pBinding->m_uColumnNumber ) {

                    _psz_convert(pBinding->m_pTargetValue, szName, pBinding->m_uBufferLength, NULL);
                    nLen = nLenTblName;
                }
                
                if ( pBinding->m_puStrLen_or_Ind ) {

                    *( pBinding->m_puStrLen_or_Ind ) = nLen;
                }
            }
        }

        nRet = SQL_SUCCESS;
    }
    
    return nRet;
}

static 
SQLRETURN _fetch_columns(TStmt * pStmt, bool bInc) {

    SQLRETURN nRet = SQL_NO_DATA;
    const int nTblNameCol = 3;
    const int nColNameCol = 4;
    const int nDataTypeCol = 5;
    const int nTypeNameCol = 6;
    const int nColSizeCol = 7;
    const char* pszVarchar = "VARCHAR";
    const int nVarcharSize = 255;
    char szSection[ODBCREST_BUFSIZ];
    char szTblName[ODBCREST_BUFSIZ];
    char szKey[ODBCREST_BUFSIZ];
    char szColName[ODBCREST_BUFSIZ];
    unsigned uRecNo = 0;
    int nPosTbl = 1;

    if ( bInc ) {

        (pStmt->m_uRecNo) ++; /** 1st */
    }
    
    while ( true ) {

        unsigned nPosCol = 1;

        sprintf(szSection, ODBCREST_TABLE, nPosTbl++ );
        if ( ! _get_ini_val( szSection, ODBCREST_NAME, szTblName, sizeof(szTblName) - 1) ) {
            break;
        }
        
        while ( true ) {

            sprintf(szKey, ODBCREST_JSONID, nPosCol++ );
            if ( ! _get_ini_val( szSection, szKey, szColName, sizeof(szColName) - 1) ) {
                break;
            }

            if ( ++uRecNo == pStmt->m_uRecNo ) {

                if ( pStmt->m_pBindRec && pStmt->m_pBindRec->m_pBinding ) {
                
                    for ( unsigned i = 0; i < pStmt->m_pBindRec->m_uNumOfBind; i ++ ) {

                        SQLLEN nLen = 0;
                        TBindCol * pBinding = &( pStmt->m_pBindRec->m_pBinding[i] );

                        if ( nTblNameCol == pBinding->m_uColumnNumber ) {

                            nLen = _psz_convert(pBinding->m_pTargetValue, szTblName, pBinding->m_uBufferLength, NULL);

                        } else if ( nColNameCol == pBinding->m_uColumnNumber ) {

                            nLen = _psz_convert(pBinding->m_pTargetValue, szColName, pBinding->m_uBufferLength, NULL);

                        } else if ( nDataTypeCol == pBinding->m_uColumnNumber ) {

                            *((SQLSMALLINT*)(pBinding->m_pTargetValue)) = SQL_VARCHAR;

                        } else if ( nTypeNameCol == pBinding->m_uColumnNumber ) {
                            
                            nLen = _psz_convert(pBinding->m_pTargetValue, pszVarchar, pBinding->m_uBufferLength, NULL);

                        } else if ( nColSizeCol == pBinding->m_uColumnNumber ) {

                            *((SQLINTEGER*)(pBinding->m_pTargetValue)) = nVarcharSize;
                        }

                        if ( pBinding->m_puStrLen_or_Ind ) {

                            *( pBinding->m_puStrLen_or_Ind ) = nLen;
                        }
                    }
                }

                nRet = SQL_SUCCESS;
                break;
            }
        }
    }
    
    return nRet;
}

/** odbc api */
static 
SQLRETURN _AllocHandle(
    SQLSMALLINT nHType,
    SQLHANDLE pSQLHandle,
    SQLHANDLE* pHandleOut) {

    SQLRETURN nRet = SQL_INVALID_HANDLE;

    if (SQL_HANDLE_ENV == nHType) {

        TEnv* env = (TEnv*)malloc(sizeof(TEnv));
        env->m_odbc_version = SQL_OV_ODBC3; /* default */
        *pHandleOut = (SQLHANDLE)env;
        nRet = SQL_SUCCESS;

    } else if (SQL_HANDLE_DBC == nHType) {

        TDbc* dbc = (TDbc*)malloc(sizeof(TDbc));
        dbc->m_pEnv = (TEnv*)pSQLHandle;
        dbc->m_bConnected = false;
        *pHandleOut = (SQLHANDLE)dbc;
        nRet = SQL_SUCCESS;

    } else if (SQL_HANDLE_STMT == nHType) {

        TStmt* stmt = (TStmt*)malloc(sizeof(TStmt));
        _init_Stmt( stmt );
        stmt->m_pDbc = (TDbc*)pSQLHandle;
        *pHandleOut = (SQLHANDLE)stmt;
        nRet = SQL_SUCCESS;
        
    } else if (SQL_HANDLE_DESC == nHType) {

        TDesc* desc = (TDesc*)malloc(sizeof(TDesc));
        desc->m_pStmt = (TStmt*)pSQLHandle;
        *pHandleOut = (SQLHANDLE)desc;
        nRet = SQL_SUCCESS;

    } else {

        nRet = SQL_ERROR;
    }


    return nRet;
}

static 
SQLRETURN _FreeHandle(
    SQLSMALLINT nHType,
    SQLHANDLE pHandle) {

    SQLRETURN nRet = SQL_INVALID_HANDLE;

    if ( pHandle ) {

        if (SQL_HANDLE_ENV == nHType) {

            free(pHandle);
        
            nRet = SQL_SUCCESS;

        } else if (SQL_HANDLE_DBC == nHType) {

            free(pHandle);
        
            nRet = SQL_SUCCESS;

        } else if (SQL_HANDLE_STMT == nHType) {

            TStmt* stmt = (TStmt*)pHandle;
            _free_Stmt( stmt );

            free(pHandle);

            nRet = SQL_SUCCESS;
            
        } else if (SQL_HANDLE_DESC == nHType) {

            free(pHandle);
        
            nRet = SQL_SUCCESS;

        } else {

            nRet = SQL_ERROR;
        }
    }

    return nRet;
}

static 
SQLRETURN _Connect(
    SQLHDBC ConnectionHandle,
    SQLCHAR* InConnectionString,
    SQLCHAR* OutConnectionString,
    SQLSMALLINT BufferLength) {

    SQLRETURN nRet = SQL_INVALID_HANDLE;
    TDbc* dbc = (TDbc*)ConnectionHandle;
    
    if ( dbc ) {
        
        dbc->m_bConnected = true;

        if (InConnectionString && OutConnectionString && (BufferLength > 0)) {

            strncpy((char*)OutConnectionString, (char*)InConnectionString, BufferLength);
        }
        
        nRet = SQL_SUCCESS;
    }   
    
    return nRet;
}

/** handles */
ODBCREST_API 
SQLRETURN SQL_API SQLAllocHandle(
    SQLSMALLINT nHType,
    SQLHANDLE pSQLHandle,
    SQLHANDLE* pHandleOut) {
   
    SQLRETURN nRet = _AllocHandle( nHType, pSQLHandle, pHandleOut );

    ODBCREST_PRINT( "SQLAllocHandle(%d,%p,%p)->%d", nHType, pSQLHandle, *pHandleOut, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLAllocEnv(
    SQLHENV *EnvironmentHandle) {

    SQLRETURN nRet = _AllocHandle( SQL_HANDLE_ENV, NULL, EnvironmentHandle );

    ODBCREST_PRINT( "SQLAllocEnv(%p)->%d", *EnvironmentHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLAllocConnect(
    SQLHENV EnvironmentHandle,
    SQLHDBC *ConnectionHandle) {

    SQLRETURN nRet = _AllocHandle( SQL_HANDLE_DBC, EnvironmentHandle, ConnectionHandle );

    ODBCREST_PRINT( "SQLAllocConnect(%p,%p)->%d", EnvironmentHandle, *ConnectionHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLAllocStmt(
    SQLHDBC ConnectionHandle,
    SQLHSTMT *StatementHandle) {

    SQLRETURN nRet = _AllocHandle( SQL_HANDLE_STMT, ConnectionHandle, StatementHandle );

    ODBCREST_PRINT( "SQLAllocStmt(%p,%p)->%d", ConnectionHandle, *StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLFreeHandle(
    SQLSMALLINT nHType,
    SQLHANDLE pHandle) {

    SQLRETURN nRet = _FreeHandle( nHType, pHandle );

    ODBCREST_PRINT( "SQLFreeHandle(%d,%p)->%d", nHType, pHandle, nRet )

    return nRet;
}

/** odbc attributes */
ODBCREST_API 
SQLRETURN SQL_API SQLSetEnvAttr(
    SQLHENV pEnv,
    SQLINTEGER nAttribute,
    SQLPOINTER pVPtr,
    SQLINTEGER nStrLength) {

    SQLRETURN nRet = SQL_INVALID_HANDLE;

    (void)nStrLength; /** @unused */

    TEnv* env = (TEnv*)pEnv;
    
    if ( env ) {

        if (SQL_ATTR_ODBC_VERSION == nAttribute) {

            unsigned version = (SQLINTEGER)(uintptr_t)pVPtr;

            /** @support ODBC 3.8 */
            if (version <= SQL_OV_ODBC3_80) {

                env->m_odbc_version = version;
                nRet = SQL_SUCCESS;

            } else {

                nRet = SQL_ERROR;
            }

        } else {

            /** nop attributes */
            nRet = SQL_SUCCESS;
        }
    }

    ODBCREST_PRINT( "SQLSetEnvAttr(%p,%d,%p,%d)->%d", pEnv, nAttribute, pVPtr, nStrLength, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLGetEnvAttr(
    SQLHENV pEnv,
    SQLINTEGER Attribute,
    SQLPOINTER ValuePtr,
    SQLINTEGER BufferLength,
    SQLINTEGER* StringLengthPtr) {

    SQLRETURN nRet = SQL_ERROR;

    (void)BufferLength; /** @unused */
    (void)StringLengthPtr; /** @unused */

    TEnv* env = (TEnv*)pEnv;
    if ((SQL_ATTR_ODBC_VERSION == Attribute) &&
         ValuePtr && pEnv) {

        *(SQLINTEGER*)ValuePtr = env->m_odbc_version;
        nRet = SQL_SUCCESS;
    }

    ODBCREST_PRINT( "SQLGetEnvAttr(%p, %d)->%d", pEnv, Attribute, nRet )
    
    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLSetConnectAttr(
    SQLHDBC ConnectionHandle,
    SQLINTEGER Attribute,
    SQLPOINTER Value,
    SQLINTEGER StringLength) {

    SQLRETURN nRet = SQL_ERROR;

    (void)ConnectionHandle; /** @unused */
    (void)Attribute; /** @unused */
    (void)Value; /** @unused */
    (void)StringLength; /** @unused */

    nRet = SQL_SUCCESS; /** SQL_AUTOCOMMIT */

    ODBCREST_PRINT( "SQLSetConnectAttr(%p,%d,%p,%d)->%d", ConnectionHandle, Attribute, Value, StringLength, nRet )
    
    return nRet;
}

/** odbc connections */
ODBCREST_API 
SQLRETURN  SQL_API SQLConnect(
    SQLHDBC ConnectionHandle,
    SQLCHAR *ServerName,
    SQLSMALLINT NameLength1,
    SQLCHAR *UserName,
    SQLSMALLINT NameLength2,
    SQLCHAR *Authentication,
    SQLSMALLINT NameLength3) {
   
    SQLRETURN nRet = _Connect( ConnectionHandle, NULL, NULL, 0 );

    (void)ConnectionHandle; /** @unused */
    (void)ServerName; /** @unused */
    (void)NameLength1; /** @unused */
    (void)UserName; /** @unused */
    (void)NameLength2; /** @unused */
    (void)Authentication; /** @unused */
    (void)NameLength3; /** @unused */

    ODBCREST_PRINT( "SQLConnect(%p)->%d", ConnectionHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLCopyDesc(
    SQLHDESC SourceDescHandle,
    SQLHDESC TargetDescHandle) {

    SQLRETURN nRet = SQL_ERROR;

    (void)SourceDescHandle; /** @unused */
    (void)TargetDescHandle; /** @unused */

    ODBCREST_PRINT( "SQLCopyDesc()->%d", nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLDriverConnect(
    SQLHDBC ConnectionHandle,
    SQLHWND WindowHandle,
    SQLCHAR* InConnectionString,
    SQLSMALLINT StringLength1,
    SQLCHAR* OutConnectionString,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* StringLength2Ptr,
    SQLUSMALLINT DriverCompletion) {

    SQLRETURN nRet = _Connect( ConnectionHandle, InConnectionString, OutConnectionString, BufferLength );

    (void)WindowHandle; /** @unused */
    (void)StringLength1; /** @unused */
    (void)StringLength2Ptr; /** @unused */
    (void)DriverCompletion; /** @unused */

    ODBCREST_PRINT( "SQLDriverConnect(%p)->%d", ConnectionHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLDisconnect(
    SQLHDBC ConnectionHandle) {
    
    SQLRETURN nRet = SQL_INVALID_HANDLE;

    if ( ConnectionHandle ) {

        TDbc* dbc = (TDbc*)ConnectionHandle;
        dbc->m_bConnected = false;
        nRet = SQL_SUCCESS;
    }
    
    ODBCREST_PRINT( "SQLDisconnect(%p)->%d", ConnectionHandle, nRet )

    return nRet;
}

/** odbc queries/metas  */
ODBCREST_API 
SQLRETURN SQL_API SQLExecDirect(
    SQLHSTMT StatementHandle,
    SQLCHAR* StatementText,
    SQLINTEGER TextLength) {

    SQLRETURN nRet = SQL_INVALID_HANDLE;

    TStmt* pStmt = (TStmt*)StatementHandle;
    if ( pStmt && StatementText ) {

        nRet = _find_and_describe_tbl(pStmt, StatementText, TextLength);
        if ( pStmt->m_nStmt > 0 ) {
            
            if ( _fetch_json( pStmt ) ) {
                nRet = SQL_SUCCESS;
            } else {
                nRet = SQL_ERROR;
            }
            pStmt->m_uRecNo = 0;
        }
    }

    ODBCREST_PRINT( "SQLExecDirect(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLNumResultCols(
    SQLHSTMT StatementHandle,
    SQLSMALLINT* ColumnCountPtr) {

    SQLRETURN nRet = SQL_INVALID_HANDLE;

    TStmt* pStmt = (TStmt*)StatementHandle;
    if ( pStmt ) {

        *ColumnCountPtr = 0;

        if ( pStmt->m_pDescRec ) {
            *ColumnCountPtr = pStmt->m_pDescRec->m_uNumOfDesc;
        }

        nRet = SQL_SUCCESS;
    }

    ODBCREST_PRINT( "SQLNumResultCols(%p,%d)->%d", StatementHandle, *ColumnCountPtr, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLDescribeCol(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT ColumnNumber,
    SQLCHAR* ColumnNamePtr,
    SQLSMALLINT BufferLength,
    SQLSMALLINT* NameLengthPtr,
    SQLSMALLINT* DataTypePtr,
    SQLULEN* ColumnSizePtr,
    SQLSMALLINT* DecimalDigitsPtr,
    SQLSMALLINT* NullablePtr) {

    SQLRETURN nRet = SQL_INVALID_HANDLE;

    TStmt* pStmt = (TStmt*)StatementHandle;
    if ( pStmt && pStmt->m_pDescRec) {

        if ( ( pStmt->m_pDescRec->m_uNumOfDesc >= ColumnNumber ) &&
             ( ColumnNumber > 0 ) ) {

            TDescCol* pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ ColumnNumber - 1 ] );

            SQLSMALLINT nLen = strlen(TO_CHAR(pDescCol->m_szColumnName));
            if ( NameLengthPtr ) {

                *NameLengthPtr = nLen;
            }

            if ( BufferLength > nLen ) {

                strcpy(TO_CHAR(ColumnNamePtr), TO_CHAR(pDescCol->m_szColumnName));
            }

            if ( DataTypePtr ) {

                *DataTypePtr = pDescCol->m_nDataType;
            }

            if ( ColumnSizePtr ) {

                *ColumnSizePtr = pDescCol->m_uColumnSize;
            }

            if ( DecimalDigitsPtr ) {

                *DecimalDigitsPtr = 0;
            }

            if ( NullablePtr ) {

                *NullablePtr = 0;
            }

            nRet = SQL_SUCCESS;
        }
    }

    ODBCREST_PRINT( "SQLDescribeCol(%p,%d)->%d", StatementHandle, ColumnNumber, nRet )

    return nRet;
}

/** odbc fetch */
ODBCREST_API 
SQLRETURN SQL_API SQLFetch(
    SQLHSTMT StatementHandle) {

    SQLRETURN nRet = SQL_NO_DATA;

    TStmt* pStmt = (TStmt*)StatementHandle;
    if ( pStmt ) {

        unsigned uRecNo = pStmt->m_uRecNo;
        if ( ODBCREST_STMT_TABLES == pStmt->m_nStmt ) {

            nRet = _fetch_tables( pStmt, true );
            
        } else if ( ODBCREST_STMT_COLUMNS == pStmt->m_nStmt ) {

            nRet = _fetch_columns( pStmt, true );

        } else {
            
            nRet = _fetch_tbl( pStmt, true );
        }

        if ( SQL_SUCCESS != nRet ) {
             pStmt->m_uRecNo = uRecNo;
        }
    }

    ODBCREST_PRINT( "SQLFetch(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLGetData(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT Col_or_Param_Num,
    SQLSMALLINT TargetType,
    SQLPOINTER TargetValuePtr,
    SQLLEN BufferLength,
    SQLLEN* StrLen_or_IndPtr) {

    SQLRETURN nRet = SQL_INVALID_HANDLE;

    TStmt* pStmt = (TStmt*)StatementHandle;
    if ( pStmt ) {

        TBindRec rec;
        TBindCol col;

        col.m_uColumnNumber = Col_or_Param_Num;
        col.m_nTargetType = TargetType;
        col.m_pTargetValue = TargetValuePtr;
        col.m_uBufferLength = BufferLength;
        col.m_puStrLen_or_Ind = StrLen_or_IndPtr;

        rec.m_uNumOfBind = 1;
        rec.m_pBinding = &col;

        TBindRec* pBindRecOrg = pStmt->m_pBindRec;
        pStmt->m_pBindRec = &rec;

        if ( ODBCREST_STMT_TABLES == pStmt->m_nStmt ) {

            nRet = _fetch_tables( pStmt, false );
            
        } else if ( ODBCREST_STMT_COLUMNS == pStmt->m_nStmt ) {

            nRet = _fetch_columns( pStmt, false );

        } else {
            
            nRet = _fetch_tbl( pStmt, false );
        }

        pStmt->m_pBindRec = pBindRecOrg;
    }
   
    ODBCREST_PRINT( "SQLGetData(%p,%d,%d,%p,%ld)->%d", StatementHandle, Col_or_Param_Num, TargetType, TargetValuePtr, BufferLength, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLSpecialColumns(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT IdentifierType,
    SQLCHAR *CatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR *SchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR *TableName,
    SQLSMALLINT NameLength3,
    SQLUSMALLINT Scope,
    SQLUSMALLINT Nullable) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)IdentifierType; /** @unused */
    (void)CatalogName; /** @unused */
    (void)NameLength1; /** @unused */
    (void)SchemaName; /** @unused */
    (void)NameLength2; /** @unused */
    (void)TableName; /** @unused */
    (void)NameLength3; /** @unused */
    (void)Scope; /** @unused */
    (void)Nullable; /** @unused */

    ODBCREST_PRINT( "SQLSpecialColumns(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLSetStmtOption(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT Option,
    SQLULEN Value) {
    
    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)Option; /** @unused */
    (void)Value; /** @unused */

    ODBCREST_PRINT( "SQLSpecialColumns(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLBindParam(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT ParameterNumber,
    SQLSMALLINT ValueType,
    SQLSMALLINT ParameterType,
    SQLULEN LengthPrecision,
    SQLSMALLINT ParameterScale,
    SQLPOINTER ParameterValue,
    SQLLEN *StrLen_or_Ind) {
    
    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)ParameterNumber; /** @unused */
    (void)ValueType; /** @unused */
    (void)ParameterType; /** @unused */
    (void)LengthPrecision; /** @unused */
    (void)ParameterScale; /** @unused */
    (void)ParameterValue; /** @unused */
    (void)StrLen_or_Ind; /** @unused */

    ODBCREST_PRINT( "SQLBindParam(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLSetPos(
    SQLHSTMT        StatementHandle,  
    SQLSETPOSIROW   RowNumber,  
    SQLUSMALLINT    Operation,  
    SQLUSMALLINT    LockType) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)RowNumber; /** @unused */
    (void)Operation; /** @unused */
    (void)LockType; /** @unused */

    ODBCREST_PRINT( "SQLSetPos(%p)->%d", StatementHandle, nRet )

    return nRet;
   
}

ODBCREST_API 
SQLRETURN SQL_API SQLStatistics(
    SQLHSTMT StatementHandle,
    SQLCHAR *CatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR *SchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR *TableName,
    SQLSMALLINT NameLength3,
    SQLUSMALLINT Unique,
    SQLUSMALLINT Reserved) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)CatalogName; /** @unused */
    (void)NameLength1; /** @unused */
    (void)SchemaName; /** @unused */
    (void)NameLength2; /** @unused */
    (void)TableName; /** @unused */
    (void)NameLength3; /** @unused */
    (void)Unique; /** @unused */
    (void)Reserved; /** @unused */

    ODBCREST_PRINT( "SQLStatistics(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLForeignKeys(
    SQLHSTMT       StatementHandle,  
    SQLCHAR *      PKCatalogName,  
    SQLSMALLINT    NameLength1,  
    SQLCHAR *      PKSchemaName,  
    SQLSMALLINT    NameLength2,  
    SQLCHAR *      PKTableName,  
    SQLSMALLINT    NameLength3,  
    SQLCHAR *      FKCatalogName,  
    SQLSMALLINT    NameLength4,  
    SQLCHAR *      FKSchemaName,  
    SQLSMALLINT    NameLength5,  
    SQLCHAR *      FKTableName,  
    SQLSMALLINT    NameLength6) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)PKCatalogName; /** @unused */
    (void)NameLength1; /** @unused */
    (void)PKSchemaName; /** @unused */
    (void)NameLength2; /** @unused */
    (void)PKTableName; /** @unused */
    (void)NameLength3; /** @unused */
    (void)FKCatalogName; /** @unused */
    (void)NameLength4; /** @unused */
    (void)FKSchemaName; /** @unused */
    (void)NameLength5; /** @unused */
    (void)FKTableName; /** @unused */
    (void)NameLength6; /** @unused */

    ODBCREST_PRINT( "SQLForeignKeys(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLGetDescRec(
    SQLHDESC DescriptorHandle,
    SQLSMALLINT RecNumber,
    SQLCHAR *Name,
    SQLSMALLINT BufferLength,
    SQLSMALLINT *StringLength,
    SQLSMALLINT *Type,
    SQLSMALLINT *SubType,
    SQLLEN *Length,
    SQLSMALLINT *Precision,
    SQLSMALLINT *Scale,
    SQLSMALLINT *Nullable) {

    SQLRETURN nRet = SQL_ERROR;

    (void)DescriptorHandle; /** @unused */
    (void)RecNumber; /** @unused */
    (void)Name; /** @unused */
    (void)BufferLength; /** @unused */
    (void)StringLength; /** @unused */
    (void)Type; /** @unused */
    (void)SubType; /** @unused */
    (void)Length; /** @unused */
    (void)Precision; /** @unused */
    (void)Scale; /** @unused */
    (void)Nullable; /** @unused */

    ODBCREST_PRINT( "SQLGetDescRec(%p)->%d", DescriptorHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLColumnPrivileges(  
    SQLHSTMT      StatementHandle,  
    SQLCHAR *     CatalogName,
    SQLSMALLINT   NameLength1,
    SQLCHAR *     SchemaName,
    SQLSMALLINT   NameLength2,
    SQLCHAR *     TableName,
    SQLSMALLINT   NameLength3,
    SQLCHAR *     ColumnName,
    SQLSMALLINT   NameLength4) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)CatalogName; /** @unused */
    (void)NameLength1; /** @unused */
    (void)SchemaName; /** @unused */
    (void)NameLength2; /** @unused */
    (void)TableName; /** @unused */
    (void)NameLength3; /** @unused */
    (void)ColumnName; /** @unused */
    (void)NameLength4; /** @unused */

    ODBCREST_PRINT( "SQLColumnPrivileges(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLRowCount(
    SQLHSTMT StatementHandle,
    SQLLEN *RowCount) {

    SQLRETURN nRet = SQL_ERROR;

    TStmt* pStmt = (TStmt*)StatementHandle;
    if ( pStmt && RowCount ) {
        
        *RowCount = pStmt->m_uRecNo;
        nRet = SQL_SUCCESS;
    }

    ODBCREST_PRINT( "SQLRowCount(%p,%ld)->%d", StatementHandle, *RowCount, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLPrepare(
    SQLHSTMT StatementHandle,
    SQLCHAR *StatementText,
    SQLINTEGER TextLength) {

    SQLRETURN nRet = SQL_INVALID_HANDLE;

    TStmt* pStmt = (TStmt*)StatementHandle;
    if ( pStmt && StatementText ) {

        nRet = _find_and_describe_tbl(pStmt, StatementText, TextLength);
    }

    ODBCREST_PRINT( "SQLPrepare(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLColumns(
    SQLHSTMT StatementHandle,
    SQLCHAR *CatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR *SchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR *TableName,
    SQLSMALLINT NameLength3,
    SQLCHAR *ColumnName,
    SQLSMALLINT NameLength4) {

    SQLRETURN nRet = SQL_ERROR;

    (void)CatalogName; /** @unused */
    (void)NameLength1; /** @unused */
    (void)SchemaName; /** @unused */
    (void)NameLength2; /** @unused */
    (void)TableName; /** @unused */
    (void)NameLength3; /** @unused */
    (void)ColumnName; /** @unused */
    (void)NameLength4; /** @unused */
    
    TStmt* pStmt = (TStmt*)StatementHandle;
    if ( pStmt ) {
        
        _free_Stmt ( pStmt );

        pStmt->m_pDescRec = (TDescRec*) malloc( sizeof( TDescRec ) );
        pStmt->m_pDescRec->m_uNumOfDesc = 18;

        pStmt->m_pDescRec->m_pDescCol = (TDescCol*) malloc(
            pStmt->m_pDescRec->m_uNumOfDesc * sizeof( TDescCol ) );

        TDescCol* pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 0 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "TABLE_CAT");
        pDescCol->m_nDataType = SQL_VARCHAR;
        pDescCol->m_uColumnSize = 255;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 1 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "TABLE_SCHEM");
        pDescCol->m_nDataType = SQL_VARCHAR;
        pDescCol->m_uColumnSize = 255;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 2 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "TABLE_NAME");
        pDescCol->m_nDataType = SQL_VARCHAR;
        pDescCol->m_uColumnSize = 255;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 3 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "COLUMN_NAME");
        pDescCol->m_nDataType = SQL_VARCHAR;
        pDescCol->m_uColumnSize = 255;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 4 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "DATA_TYPE");
        pDescCol->m_nDataType = SQL_INTEGER; //SQL_SMALLINT;
        pDescCol->m_uColumnSize = 4;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 5 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "TYPE_NAME");
        pDescCol->m_nDataType = SQL_SMALLINT;
        pDescCol->m_uColumnSize = 255;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 6 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "COLUMN_SIZE");
        pDescCol->m_nDataType = SQL_INTEGER;
        pDescCol->m_uColumnSize = 0;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 7 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "BUFFER_LENGTH");
        pDescCol->m_nDataType = SQL_INTEGER;
        pDescCol->m_uColumnSize = 0;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 8 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "DECIMAL_DIGITS");
        pDescCol->m_nDataType = SQL_SMALLINT;
        pDescCol->m_uColumnSize = 0;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 9 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "NUM_PREC_RADIX");
        pDescCol->m_nDataType = SQL_SMALLINT;
        pDescCol->m_uColumnSize = 0;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 10 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "NULLABLE");
        pDescCol->m_nDataType = SQL_SMALLINT;
        pDescCol->m_uColumnSize = 0;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 11 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "REMARKS");
        pDescCol->m_nDataType = SQL_VARCHAR;
        pDescCol->m_uColumnSize = 255;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 12 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "COLUMN_DEF");
        pDescCol->m_nDataType = SQL_VARCHAR;
        pDescCol->m_uColumnSize = 255;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 13 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "SQL_DATA_TYPE");
        pDescCol->m_nDataType = SQL_SMALLINT;
        pDescCol->m_uColumnSize = 0;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 14 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "SQL_DATETIME_SUB");
        pDescCol->m_nDataType = SQL_SMALLINT;
        pDescCol->m_uColumnSize = 0;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 15 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "CHAR_OCTET_LENGTH");
        pDescCol->m_nDataType = SQL_INTEGER;
        pDescCol->m_uColumnSize = 0;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 16 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "ORDINAL_POSITION");
        pDescCol->m_nDataType = SQL_INTEGER;
        pDescCol->m_uColumnSize = 0;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 17 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "IS_NULLABLE");
        pDescCol->m_nDataType = SQL_VARCHAR;
        pDescCol->m_uColumnSize = 255;

        pStmt->m_nStmt = ODBCREST_STMT_COLUMNS;
        pStmt->m_uRecNo = 0;
        nRet = SQL_SUCCESS;
    }

    ODBCREST_PRINT( "SQLColumns(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLSetScrollOptions(
    SQLHSTMT      StatementHandle,
    SQLUSMALLINT       fConcurrency,
    SQLLEN             crowKeyset,
    SQLUSMALLINT       crowRowset) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)fConcurrency; /** @unused */
    (void)crowKeyset; /** @unused */
    (void)crowRowset; /** @unused */

    ODBCREST_PRINT( "SQLSetScrollOptions(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLBrowseConnect(
    SQLHDBC            hdbc,
    SQLCHAR           *szConnStrIn,
    SQLSMALLINT        cbConnStrIn,
    SQLCHAR           *szConnStrOut,
    SQLSMALLINT        cbConnStrOutMax,
    SQLSMALLINT       *pcbConnStrOut) {

    SQLRETURN nRet = SQL_ERROR;

    (void)hdbc; /** @unused */
    (void)szConnStrIn; /** @unused */
    (void)cbConnStrIn; /** @unused */
    (void)szConnStrOut; /** @unused */
    (void)cbConnStrOutMax; /** @unused */
    (void)pcbConnStrOut; /** @unused */

    ODBCREST_PRINT( "SQLBrowseConnect(%p)->%d", hdbc, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLNativeSql(
    SQLHDBC            hdbc,
    SQLCHAR           *szSqlStrIn,
    SQLINTEGER         cbSqlStrIn,
    SQLCHAR           *szSqlStr,
    SQLINTEGER         cbSqlStrMax,
    SQLINTEGER           *pcbSqlStr) {

    SQLRETURN nRet = SQL_ERROR;

    (void)hdbc; /** @unused */
    (void)szSqlStrIn; /** @unused */
    (void)cbSqlStrIn; /** @unused */
    (void)szSqlStr; /** @unused */
    (void)cbSqlStrMax; /** @unused */
    (void)pcbSqlStr; /** @unused */

    ODBCREST_PRINT( "SQLNativeSql(%p)->%d", hdbc, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLGetDescField(
    SQLHDESC DescriptorHandle,
    SQLSMALLINT RecNumber,
    SQLSMALLINT FieldIdentifier,
    SQLPOINTER Value,
    SQLINTEGER BufferLength,
    SQLINTEGER *StringLength) {

    SQLRETURN nRet = SQL_ERROR;

    (void)DescriptorHandle; /** @unused */
    (void)RecNumber; /** @unused */
    (void)FieldIdentifier; /** @unused */
    (void)Value; /** @unused */
    (void)BufferLength; /** @unused */
    (void)StringLength; /** @unused */

    ODBCREST_PRINT( "SQLGetDescField(%p)->%d", DescriptorHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLGetDiagField(
    SQLSMALLINT HandleType,
    SQLHANDLE Handle,
    SQLSMALLINT RecNumber,
    SQLSMALLINT DiagIdentifier,
    SQLPOINTER DiagInfo,
    SQLSMALLINT BufferLength,
    SQLSMALLINT *StringLength) {

    SQLRETURN nRet = SQL_ERROR;

    (void)HandleType; /** @unused */
    (void)Handle; /** @unused */
    (void)RecNumber; /** @unused */
    (void)DiagIdentifier; /** @unused */
    (void)DiagInfo; /** @unused */
    (void)BufferLength; /** @unused */
    (void)StringLength; /** @unused */

    ODBCREST_PRINT( "SQLGetDiagField(%d,%p)->%d", HandleType, Handle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLGetDiagRec(
    SQLSMALLINT HandleType,
    SQLHANDLE Handle,
    SQLSMALLINT RecNumber,
    SQLCHAR *Sqlstate,
    SQLINTEGER *NativeError,
    SQLCHAR *MessageText,
    SQLSMALLINT BufferLength,
    SQLSMALLINT *TextLength) {

    SQLRETURN nRet = SQL_ERROR;

    if (SQL_HANDLE_STMT == HandleType) {
        
        TStmt* pStmt = (TStmt*)Handle;
        if ( pStmt && MessageText && strlen(pStmt->m_szErrMsg) ) {
    
            if ( 1 == RecNumber ) {

                SQLINTEGER nBufferLength = BufferLength;
                SQLINTEGER nStringLength;
                _psz_convert(
                    MessageText,
                    pStmt->m_szErrMsg,
                    nBufferLength,
                    &nStringLength);

                if ( TextLength ) {
                    *TextLength = nStringLength;
                }

                if ( Sqlstate ) {
                    strcpy(TO_CHAR(Sqlstate), "00001");
                }

                if ( NativeError ) {
                    *NativeError = 1;
                }

                nRet = SQL_SUCCESS;

            } else {

                nRet = SQL_NO_DATA;
            }
        }
    }

    ODBCREST_PRINT( "SQLGetDiagRec(%d,%p,%d)->%d", HandleType, Handle, RecNumber, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLFetchScroll(
    SQLHSTMT StatementHandle,
    SQLSMALLINT FetchOrientation,
    SQLLEN FetchOffset) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)FetchOrientation; /** @unused */
    (void)FetchOffset; /** @unused */

    TStmt* pStmt = (TStmt*)StatementHandle;
    if ( pStmt && (1 == FetchOrientation) && (1 == FetchOffset) ) {

        unsigned uRecNo = pStmt->m_uRecNo;
        if ( ODBCREST_STMT_TABLES == pStmt->m_nStmt ) {

            nRet = _fetch_tables( pStmt, true );
            
        } else if ( ODBCREST_STMT_COLUMNS == pStmt->m_nStmt ) {

            nRet = _fetch_columns( pStmt, true );

        } else {
            
            nRet = _fetch_tbl( pStmt, true );
        }

        if ( SQL_SUCCESS != nRet ) {
             pStmt->m_uRecNo = uRecNo;
        }
    }

    ODBCREST_PRINT( "SQLFetchScroll(%p,%d,%ld)->%d", StatementHandle, FetchOrientation, FetchOffset, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLExecute(
    SQLHSTMT StatementHandle) {

    SQLRETURN nRet = SQL_ERROR;

    TStmt* pStmt = (TStmt*)StatementHandle;
    if ( pStmt ) {

        if ( pStmt->m_nStmt > 0 ) {
            
            if ( _fetch_json( pStmt ) ) {

                pStmt->m_uRecNo = 0;
                nRet = SQL_SUCCESS;
            }

        } else {

            nRet = SQL_SUCCESS; /** nop */
        }
    }

    ODBCREST_PRINT( "SQLExecute(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLParamOptions(
    SQLHSTMT           hstmt,
    SQLULEN               crow,
    SQLULEN              *pirow) {

    SQLRETURN nRet = SQL_ERROR;

    (void)hstmt; /** @unused */
    (void)crow; /** @unused */
    (void)pirow; /** @unused */

    ODBCREST_PRINT( "SQLParamOptions(%p)->%d", hstmt, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLSetDescField(
    SQLHDESC DescriptorHandle,
    SQLSMALLINT RecNumber,
    SQLSMALLINT FieldIdentifier,
    SQLPOINTER Value,
    SQLINTEGER BufferLength) {

    SQLRETURN nRet = SQL_ERROR;

    (void)DescriptorHandle; /** @unused */
    (void)RecNumber; /** @unused */
    (void)FieldIdentifier; /** @unused */
    (void)Value; /** @unused */
    (void)BufferLength; /** @unused */

    ODBCREST_PRINT( "SQLSetDescField(%p)->%d", DescriptorHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLSetDescRec(
    SQLHDESC DescriptorHandle,
    SQLSMALLINT RecNumber,
    SQLSMALLINT Type,
    SQLSMALLINT SubType,
    SQLLEN Length,
    SQLSMALLINT Precision,
    SQLSMALLINT Scale,
    SQLPOINTER Data,
    SQLLEN *StringLength,
    SQLLEN *Indicator) {

    SQLRETURN nRet = SQL_ERROR;

    (void)DescriptorHandle; /** @unused */
    (void)RecNumber; /** @unused */
    (void)Type; /** @unused */
    (void)SubType; /** @unused */
    (void)Length; /** @unused */
    (void)Precision; /** @unused */
    (void)Scale; /** @unused */
    (void)Data; /** @unused */
    (void)StringLength; /** @unused */
    (void)Indicator; /** @unused */

    ODBCREST_PRINT( "SQLSetDescRec(%p)->%d", DescriptorHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLDataSources(
    SQLHENV EnvironmentHandle,
    SQLUSMALLINT Direction,
    SQLCHAR *ServerName,
    SQLSMALLINT BufferLength1,
    SQLSMALLINT *NameLength1,
    SQLCHAR *Description,
    SQLSMALLINT BufferLength2,
    SQLSMALLINT *NameLength2) {

    SQLRETURN nRet = SQL_ERROR;

    (void)EnvironmentHandle; /** @unused */
    (void)Direction; /** @unused */
    (void)ServerName; /** @unused */
    (void)BufferLength1; /** @unused */
    (void)NameLength1; /** @unused */
    (void)Description; /** @unused */
    (void)BufferLength2; /** @unused */
    (void)NameLength2; /** @unused */

    ODBCREST_PRINT( "SQLDataSources(%p)->%d", EnvironmentHandle, nRet )

    return nRet;
}

#ifdef ODBCREST_FUN
ODBCREST_API 
SQLRETURN SQL_API SQLGetFunctions(
    SQLHDBC ConnectionHandle,
    SQLUSMALLINT FunctionId,
    SQLUSMALLINT *Supported) {

    SQLRETURN nRet = SQL_ERROR;
    static SQLUSMALLINT s_supported[256];
    for (int i = 0; i < 256; i ++ ) s_supported[ i ] = 0;

    (void)ConnectionHandle; /** @unused */
    (void)FunctionId; /** @unused */
    (void)Supported; /** @unused */

    nRet = SQL_SUCCESS;
    Supported = &( s_supported[ 0 ] );

    ODBCREST_PRINT( "SQLGetFunctions(%p,%d,%d)->%d", ConnectionHandle, FunctionId, *Supported, nRet )

    return nRet;
}
#endif // ODBCREST_FUN

ODBCREST_API 
SQLRETURN SQL_API SQLProcedures(
    SQLHSTMT           hstmt,
    SQLCHAR           *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLCHAR           *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLCHAR           *szProcName,
    SQLSMALLINT        cbProcName) {

    SQLRETURN nRet = SQL_ERROR;

    (void)hstmt; /** @unused */
    (void)szCatalogName; /** @unused */
    (void)cbCatalogName; /** @unused */
    (void)szSchemaName; /** @unused */
    (void)cbSchemaName; /** @unused */
    (void)szProcName; /** @unused */
    (void)cbProcName; /** @unused */

    ODBCREST_PRINT( "SQLProcedures(%p)->%d", hstmt, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLProcedureColumns(
    SQLHSTMT           hstmt,
    SQLCHAR           *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLCHAR           *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLCHAR           *szProcName,
    SQLSMALLINT        cbProcName,
    SQLCHAR           *szColumnName,
    SQLSMALLINT        cbColumnName) {

    SQLRETURN nRet = SQL_ERROR;

    (void)hstmt; /** @unused */
    (void)szCatalogName; /** @unused */
    (void)cbCatalogName; /** @unused */
    (void)szSchemaName; /** @unused */
    (void)cbSchemaName; /** @unused */
    (void)szProcName; /** @unused */
    (void)cbProcName; /** @unused */
    (void)szColumnName; /** @unused */
    (void)cbColumnName; /** @unused */

    ODBCREST_PRINT( "SQLProcedureColumns(%p)->%d", hstmt, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLGetInfo(
    SQLHDBC ConnectionHandle,
    SQLUSMALLINT InfoType,
    SQLPOINTER InfoValue,
    SQLSMALLINT BufferLength,
    SQLSMALLINT *StringLength) {

    SQLRETURN nRet = SQL_ERROR;

    (void)ConnectionHandle; /** @unused */
    
    if ( (SQL_DRIVER_ODBC_VER == InfoType) && (BufferLength > 4) ) {

        sprintf(InfoValue, "%ld.00", SQL_OV_ODBC3);
        *StringLength = 5;
        nRet = SQL_SUCCESS;

    } else if ( SQL_DESCRIBE_PARAMETER == InfoType ) {

        /** NO */
        nRet = SQL_SUCCESS;

    } else if ( SQL_NEED_LONG_DATA_LEN == InfoType ) {

        /** NO */
        nRet = SQL_SUCCESS;
        
    } else if ( SQL_CURSOR_COMMIT_BEHAVIOR == InfoType ) {

        *((SQLUINTEGER*)InfoValue) = SQL_CB_DELETE;
        nRet = SQL_SUCCESS;

    } else if ( SQL_CURSOR_ROLLBACK_BEHAVIOR == InfoType ) {

        *((SQLUINTEGER*)InfoValue) = SQL_CB_DELETE;
        nRet = SQL_SUCCESS;

    } else if ( SQL_SQL92_VALUE_EXPRESSIONS == InfoType ) {

        /** NO */
        *((SQLUINTEGER*)InfoValue) = 0;
        *StringLength = sizeof(SQLUINTEGER);
        nRet = SQL_SUCCESS;

    /*} else if ( SQL_OWNER_USAGE == InfoType ) {

        *((SQLSMALLINT*)InfoValue) = 0;
        *StringLength = sizeof(SQLSMALLINT);
        nRet = SQL_SUCCESS;

    } else if ( SQL_QUALIFIER_USAGE == InfoType ) {

        *((SQLSMALLINT*)InfoValue) = 0;
        *StringLength = sizeof(SQLSMALLINT);
        nRet = SQL_SUCCESS;

    } else if ( SQL_QUOTED_IDENTIFIER_CASE == InfoType ) {

        *((SQLSMALLINT*)InfoValue) = SQL_IC_UPPER;
        *StringLength = sizeof(SQLSMALLINT);
        nRet = SQL_SUCCESS;

    } else if ( SQL_CATALOG_NAME_SEPARATOR == InfoType ) {

        *((SQLUINTEGER*)InfoValue) = '.';
        *StringLength = sizeof(SQLUINTEGER);
        nRet = SQL_SUCCESS;

    } else if ( SQL_IDENTIFIER_QUOTE_CHAR == InfoType ) {


        *((SQLUINTEGER*)InfoValue) = '\'';
        *StringLength = sizeof(SQLUINTEGER);
        nRet = SQL_SUCCESS;

    */} else {
        nRet = SQL_SUCCESS; /** default */
    }

    ODBCREST_PRINT( "SQLGetInfo(%p,%d)->%d", ConnectionHandle, InfoType, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLSetConnectOption(
    SQLHDBC ConnectionHandle,
    SQLUSMALLINT Option,
    SQLULEN Value) {

    SQLRETURN nRet = SQL_ERROR;

    (void)ConnectionHandle; /** @unused */
    (void)Option; /** @unused */
    (void)Value; /** @unused */

    ODBCREST_PRINT( "SQLSetConnectOption(%p)->%d", ConnectionHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLSetCursorName(
    SQLHSTMT StatementHandle,
    SQLCHAR *CursorName,
    SQLSMALLINT NameLength) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)CursorName; /** @unused */
    (void)NameLength; /** @unused */

    ODBCREST_PRINT( "SQLSetCursorName(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLTablePrivileges(
    SQLHSTMT           hstmt,
    SQLCHAR           *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLCHAR           *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLCHAR           *szTableName,
    SQLSMALLINT        cbTableName) {

    SQLRETURN nRet = SQL_ERROR;

    (void)hstmt; /** @unused */
    (void)szCatalogName; /** @unused */
    (void)cbCatalogName; /** @unused */
    (void)szSchemaName; /** @unused */
    (void)cbSchemaName; /** @unused */
    (void)szTableName; /** @unused */
    (void)cbTableName; /** @unused */

    ODBCREST_PRINT( "SQLTablePrivileges(%p)->%d", hstmt, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLDrivers(
    SQLHENV            henv,
    SQLUSMALLINT       fDirection,
    SQLCHAR           *szDriverDesc,
    SQLSMALLINT        cbDriverDescMax,
    SQLSMALLINT       *pcbDriverDesc,
    SQLCHAR           *szDriverAttributes,
    SQLSMALLINT        cbDrvrAttrMax,
    SQLSMALLINT       *pcbDrvrAttr) {

    SQLRETURN nRet = SQL_ERROR;

    (void)henv; /** @unused */
    (void)fDirection; /** @unused */
    (void)szDriverDesc; /** @unused */
    (void)cbDriverDescMax; /** @unused */
    (void)pcbDriverDesc; /** @unused */
    (void)szDriverAttributes; /** @unused */
    (void)cbDrvrAttrMax; /** @unused */
    (void)pcbDrvrAttr; /** @unused */

    ODBCREST_PRINT( "SQLDrivers(%p)->%d", henv, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLBindParameter(
    SQLHSTMT           hstmt,
    SQLUSMALLINT       ipar,
    SQLSMALLINT        fParamType,
    SQLSMALLINT        fCType,
    SQLSMALLINT        fSqlType,
    SQLULEN            cbColDef,
    SQLSMALLINT        ibScale,
    SQLPOINTER         rgbValue,
    SQLLEN             cbValueMax,
    SQLLEN             *pcbValue) {

    SQLRETURN nRet = SQL_ERROR;

    (void)hstmt; /** @unused */
    (void)ipar; /** @unused */
    (void)fParamType; /** @unused */
    (void)fCType; /** @unused */
    (void)fSqlType; /** @unused */
    (void)cbColDef; /** @unused */
    (void)ibScale; /** @unused */
    (void)rgbValue; /** @unused */
    (void)cbValueMax; /** @unused */
    (void)pcbValue; /** @unused */

    ODBCREST_PRINT( "SQLBindParameter(%p)->%d", hstmt, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLPutData(
    SQLHSTMT StatementHandle,
    SQLPOINTER Data, 
    SQLLEN StrLen_or_Ind) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)Data; /** @unused */
    (void)StrLen_or_Ind; /** @unused */

    ODBCREST_PRINT( "SQLPutData(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLCancel(
    SQLHSTMT StatementHandle) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */

    ODBCREST_PRINT( "SQLCancel(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLFreeEnv(
    SQLHENV EnvironmentHandle) {

    SQLRETURN nRet = _FreeHandle( SQL_HANDLE_ENV, EnvironmentHandle );

    ODBCREST_PRINT( "SQLFreeEnv(%p)->%d", EnvironmentHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLFreeConnect(
    SQLHDBC ConnectionHandle) {

    SQLRETURN nRet = _FreeHandle( SQL_HANDLE_DBC, ConnectionHandle );

    ODBCREST_PRINT( "SQLFreeConnect(%p)->%d", ConnectionHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLFreeStmt(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT Option) {

    SQLRETURN nRet = SQL_INVALID_HANDLE;

    if ( SQL_CLOSE == Option ) {

        /** drop select results */
        /*_free_Stmt((TStmt*) StatementHandle);*/
        TStmt* pStmt = (TStmt*)StatementHandle;
        if ( pStmt ) {
            if ( pStmt->m_pJsonObj ) {
                json_object_put( pStmt->m_pJsonObj );
            }
            pStmt->m_pJsonObj = NULL;
            pStmt->m_pJsonRes = NULL;
        }
        nRet = SQL_SUCCESS;

    } else if ( SQL_DROP == Option  ) {

        nRet = _FreeHandle( SQL_HANDLE_STMT, StatementHandle );

    } else if ( SQL_UNBIND == Option  ) {

        /** drop bind-cols */
        _free_Stmt((TStmt*) StatementHandle);
        nRet = SQL_SUCCESS;

    } else if ( SQL_RESET_PARAMS == Option  ) {

        /** drop bind-params */
        _free_Stmt((TStmt*) StatementHandle);
        nRet = SQL_SUCCESS;
    }
    
    ODBCREST_PRINT( "SQLFreeStmt(%p,%d)->%d", StatementHandle, Option, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLGetCursorName(
    SQLHSTMT StatementHandle,
    SQLCHAR *CursorName,
    SQLSMALLINT BufferLength,
    SQLSMALLINT *NameLength) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)CursorName; /** @unused */
    (void)BufferLength; /** @unused */
    (void)NameLength; /** @unused */

    ODBCREST_PRINT( "SQLGetCursorName(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLMoreResults(
    SQLHSTMT hstmt) {

    SQLRETURN nRet = SQL_NO_DATA;

    (void)hstmt; /** @unused */

    ODBCREST_PRINT( "SQLMoreResults(%p)->%d", hstmt, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLNumParams(
    SQLHSTMT hstmt,
    SQLSMALLINT *pcpar) {

    SQLRETURN nRet = SQL_ERROR;

    TStmt* pStmt = (TStmt*)hstmt;
    if ( pStmt ) {
        
        *pcpar = 0;

        nRet = SQL_SUCCESS;
    }

    ODBCREST_PRINT( "SQLNumParams(%p,%d)->%d", hstmt, *pcpar, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLExtendedFetch(
    SQLHSTMT hstmt,
    SQLUSMALLINT fFetchType,
    SQLLEN irow,
    SQLULEN *pcrow,
    SQLUSMALLINT *rgfRowStatus) {

    SQLRETURN nRet = SQL_ERROR;

    (void)hstmt; /** @unused */
    (void)fFetchType; /** @unused */
    (void)irow; /** @unused */
    (void)pcrow; /** @unused */
    (void)rgfRowStatus; /** @unused */

    ODBCREST_PRINT( "SQLExtendedFetch(%p)->%d", hstmt, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLGetStmtOption(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT Option,
    SQLPOINTER Value) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)Option; /** @unused */
    (void)Value; /** @unused */

    ODBCREST_PRINT( "SQLGetStmtOption(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLColAttributes(
    SQLHSTMT hstmt,
    SQLUSMALLINT icol,
    SQLUSMALLINT fDescType,
    SQLPOINTER rgbDesc,
    SQLSMALLINT cbDescMax,
    SQLSMALLINT *pcbDesc,
    SQLLEN *pfDesc) {

    SQLRETURN nRet = SQL_ERROR;

    (void)hstmt; /** @unused */
    (void)icol; /** @unused */
    (void)fDescType; /** @unused */
    (void)rgbDesc; /** @unused */
    (void)cbDescMax; /** @unused */
    (void)pcbDesc; /** @unused */
    (void)pfDesc; /** @unused */

    ODBCREST_PRINT( "SQLExtendedFetch(%p)->%d", hstmt, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLGetStmtAttr(
    SQLHSTMT StatementHandle,
    SQLINTEGER Attribute,
    SQLPOINTER Value,
    SQLINTEGER BufferLength,
    SQLINTEGER *StringLength) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)Attribute; /** @unused */
    (void)Value; /** @unused */
    (void)BufferLength; /** @unused */
    (void)StringLength; /** @unused */
    
    if ( SQL_ATTR_APP_ROW_DESC == Attribute ) {

        nRet = SQL_SUCCESS;

    } else if ( SQL_ATTR_APP_PARAM_DESC == Attribute ) {

        nRet = SQL_SUCCESS;

    } else if ( SQL_ATTR_IMP_ROW_DESC == Attribute ) {

        nRet = SQL_SUCCESS;

    } else if ( SQL_ATTR_IMP_PARAM_DESC == Attribute ) {

        nRet = SQL_SUCCESS;
    } else {
        nRet = SQL_SUCCESS; /** default */
    }

    ODBCREST_PRINT( "SQLGetStmtAttr(%p,%d)->%d", StatementHandle, Attribute, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLParamData(
    SQLHSTMT StatementHandle,
    SQLPOINTER *Value) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)Value; /** @unused */

    ODBCREST_PRINT( "SQLParamData(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLGetConnectOption(
    SQLHDBC ConnectionHandle,
    SQLUSMALLINT Option,
    SQLPOINTER Value) {

    SQLRETURN nRet = SQL_ERROR;

    (void)ConnectionHandle; /** @unused */
    (void)Option; /** @unused */
    (void)Value; /** @unused */

    ODBCREST_PRINT( "SQLGetConnectOption(%p)->%d", ConnectionHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLPrimaryKeys(
    SQLHSTMT hstmt,
    SQLCHAR *szCatalogName,
    SQLSMALLINT cbCatalogName,
    SQLCHAR *szSchemaName,
    SQLSMALLINT cbSchemaName,
    SQLCHAR *szTableName,
    SQLSMALLINT cbTableName) {

    SQLRETURN nRet = SQL_ERROR;

    (void)hstmt; /** @unused */
    (void)szCatalogName; /** @unused */
    (void)cbCatalogName; /** @unused */
    (void)szSchemaName; /** @unused */
    (void)cbSchemaName; /** @unused */
    (void)szTableName; /** @unused */
    (void)cbTableName; /** @unused */

    ODBCREST_PRINT( "SQLPrimaryKeys(%p)->%d", hstmt, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLError(
    SQLHENV EnvironmentHandle,
    SQLHDBC ConnectionHandle,
    SQLHSTMT StatementHandle,
    SQLCHAR *Sqlstate,
    SQLINTEGER *NativeError,
    SQLCHAR *MessageText,
    SQLSMALLINT BufferLength,
    SQLSMALLINT *TextLength) {

    SQLRETURN nRet = SQL_ERROR;

    (void)EnvironmentHandle; /** @unused */
    (void)ConnectionHandle; /** @unused */
    (void)Sqlstate; /** @unused */
    (void)NativeError; /** @unused */

    TStmt* pStmt = (TStmt*)StatementHandle;
    if ( pStmt ) {

        unsigned uLen = strlen(pStmt->m_szErrMsg);
        if ( uLen ) {

            SQLINTEGER nBufferLength = BufferLength;
            SQLINTEGER* pStringLengthPtr = NULL;

            _psz_convert(
                MessageText,
                pStmt->m_szErrMsg,
                nBufferLength,
                pStringLengthPtr);

            if ( TextLength ) {
                *TextLength = (SQLSMALLINT)(*pStringLengthPtr);
            }
        }
    }

    ODBCREST_PRINT( "SQLError(%p,%p,%p)->%d", EnvironmentHandle, ConnectionHandle, StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLGetConnectAttr(
    SQLHDBC ConnectionHandle,
    SQLINTEGER Attribute,
    SQLPOINTER Value,
    SQLINTEGER BufferLength,
    SQLINTEGER *StringLength) {

    SQLRETURN nRet = SQL_ERROR;

    (void)ConnectionHandle; /** @unused */
    (void)Attribute; /** @unused */
    (void)Value; /** @unused */
    (void)BufferLength; /** @unused */
    (void)StringLength; /** @unused */

    ODBCREST_PRINT( "SQLGetConnectAttr(%p)->%d", ConnectionHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLBulkOperations(
    SQLHSTMT StatementHandle,
    SQLSMALLINT Operation) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)Operation; /** @unused */

    ODBCREST_PRINT( "SQLBulkOperations(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLTables(
    SQLHSTMT StatementHandle,
    SQLCHAR *CatalogName,
    SQLSMALLINT NameLength1,
    SQLCHAR *SchemaName,
    SQLSMALLINT NameLength2,
    SQLCHAR *TableName,
    SQLSMALLINT NameLength3,
    SQLCHAR *TableType,
    SQLSMALLINT NameLength4) {

    SQLRETURN nRet = SQL_INVALID_HANDLE;

    (void)CatalogName; /** @unused */
    (void)NameLength1; /** @unused */
    (void)SchemaName; /** @unused */
    (void)NameLength2; /** @unused */
    (void)TableName; /** @unused */
    (void)NameLength3; /** @unused */
    (void)TableType; /** @unused */
    (void)NameLength4; /** @unused */

    TStmt* pStmt = (TStmt*)StatementHandle;
    if ( pStmt ) {
        
        _free_Stmt ( pStmt );

        pStmt->m_pDescRec = (TDescRec*) malloc( sizeof( TDescRec ) );
        pStmt->m_pDescRec->m_uNumOfDesc = 5;

        pStmt->m_pDescRec->m_pDescCol = (TDescCol*) malloc(
            pStmt->m_pDescRec->m_uNumOfDesc * sizeof( TDescCol ) );

        TDescCol* pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 0 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "TABLE_CAT");
        pDescCol->m_nDataType = SQL_VARCHAR;
        pDescCol->m_uColumnSize = 255;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 1 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "TABLE_SCHEM");
        pDescCol->m_nDataType = SQL_VARCHAR;
        pDescCol->m_uColumnSize = 255;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 2 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "TABLE_NAME");
        pDescCol->m_nDataType = SQL_VARCHAR;
        pDescCol->m_uColumnSize = 255;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 3 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "TABLE_TYPE");
        pDescCol->m_nDataType = SQL_VARCHAR;
        pDescCol->m_uColumnSize = 255;

        pDescCol = &( pStmt->m_pDescRec->m_pDescCol[ 4 ] );
        strcpy( TO_CHAR(pDescCol->m_szColumnName), "REMARKS");
        pDescCol->m_nDataType = SQL_VARCHAR;
        pDescCol->m_uColumnSize = 255;

        pStmt->m_nStmt = ODBCREST_STMT_TABLES;
        pStmt->m_uRecNo = 0;
        nRet = SQL_SUCCESS;
    }
    
    ODBCREST_PRINT( "SQLTables(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLCloseCursor(
    SQLHSTMT StatementHandle) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */

    ODBCREST_PRINT( "SQLCloseCursor(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLColAttribute(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT ColumnNumber,
    SQLUSMALLINT FieldIdentifier,
    SQLPOINTER CharacterAttribute,
    SQLSMALLINT BufferLength,
    SQLSMALLINT *StringLength,
    SQLLEN *NumericAttribute ) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)ColumnNumber; /** @unused */
    (void)FieldIdentifier; /** @unused */
    (void)CharacterAttribute; /** @unused */
    (void)BufferLength; /** @unused */
    (void)StringLength; /** @unused */
    (void)NumericAttribute; /** @unused */
    
    nRet = SQL_SUCCESS; /** nop */

    ODBCREST_PRINT( "SQLColAttribute(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLDescribeParam(
    SQLHSTMT hstmt,
    SQLUSMALLINT ipar,
    SQLSMALLINT *pfSqlType,
    SQLULEN *pcbParamDef,
    SQLSMALLINT *pibScale,
    SQLSMALLINT *pfNullable) {

    SQLRETURN nRet = SQL_ERROR;

    (void)hstmt; /** @unused */
    (void)ipar; /** @unused */
    (void)pfSqlType; /** @unused */
    (void)pcbParamDef; /** @unused */
    (void)pibScale; /** @unused */
    (void)pfNullable; /** @unused */

    ODBCREST_PRINT( "SQLDescribeParam(%p)->%d", hstmt, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLSetParam(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT ParameterNumber,
    SQLSMALLINT ValueType,
    SQLSMALLINT ParameterType,
    SQLULEN LengthPrecision,
    SQLSMALLINT ParameterScale,
    SQLPOINTER ParameterValue,
    SQLLEN *StrLen_or_Ind) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)ParameterNumber; /** @unused */
    (void)ValueType; /** @unused */
    (void)ParameterType; /** @unused */
    (void)LengthPrecision; /** @unused */
    (void)ParameterScale; /** @unused */
    (void)ParameterValue; /** @unused */
    (void)StrLen_or_Ind; /** @unused */
 
    ODBCREST_PRINT( "SQLSetParam(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLBindCol(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT ColumnNumber,
    SQLSMALLINT TargetType,
    SQLPOINTER TargetValue,
    SQLLEN BufferLength,
    SQLLEN *StrLen_or_Ind) {

    SQLRETURN nRet = SQL_ERROR;

    TStmt* pStmt = (TStmt*)StatementHandle;
    if ( pStmt ) {

        TBindCol* pBindCol = NULL;
        if ( pStmt->m_pBindRec && pStmt->m_pBindRec->m_pBinding ) {
            
            unsigned uNumOfBind = pStmt->m_pBindRec->m_uNumOfBind;
            pStmt->m_pBindRec->m_pBinding = (TBindCol*)
                _ext_mem(pStmt->m_pBindRec->m_pBinding,
                         sizeof(TBindCol) * uNumOfBind,
                         sizeof(TBindCol) * (uNumOfBind + 1));
            pStmt->m_pBindRec->m_uNumOfBind ++;
            pBindCol = &(pStmt->m_pBindRec->m_pBinding[
                pStmt->m_pBindRec->m_uNumOfBind - 1]);

        } else {
            
            pStmt->m_pBindRec = (TBindRec*) malloc( sizeof( TBindRec ) );
            pStmt->m_pBindRec->m_uNumOfBind = 1;
            pStmt->m_pBindRec->m_pBinding = (TBindCol*) malloc( sizeof( TBindCol ) );
            pBindCol = pStmt->m_pBindRec->m_pBinding;
        }

        pBindCol->m_uColumnNumber = ColumnNumber;
        pBindCol->m_nTargetType = TargetType;
        pBindCol->m_pTargetValue = TargetValue;
        pBindCol->m_uBufferLength = BufferLength;
        pBindCol->m_puStrLen_or_Ind = StrLen_or_Ind;

        nRet = SQL_SUCCESS;      
    }
 
    ODBCREST_PRINT( "SQLBindCol(%p,%d)->%d", StatementHandle, ColumnNumber, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLSetStmtAttr(
    SQLHSTMT StatementHandle,
    SQLINTEGER Attribute,
    SQLPOINTER Value,
    SQLINTEGER StringLength) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)Attribute; /** @unused */
    (void)Value; /** @unused */
    (void)StringLength; /** @unused */

    nRet = SQL_SUCCESS; /** nop */
 
    ODBCREST_PRINT( "SQLSetStmtAttr(%p)->%d", StatementHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLGetTypeInfo(
    SQLHSTMT StatementHandle,
    SQLSMALLINT DataType) {

    SQLRETURN nRet = SQL_ERROR;

    (void)StatementHandle; /** @unused */
    (void)DataType; /** @unused */

    nRet = SQL_SUCCESS;
 
    ODBCREST_PRINT( "SQLGetTypeInfo(%p,%d)->%d", StatementHandle, DataType, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLTransact(
    SQLHENV EnvironmentHandle,
    SQLHDBC ConnectionHandle,
    SQLUSMALLINT CompletionType) {

    SQLRETURN nRet = SQL_ERROR;

    (void)EnvironmentHandle; /** @unused */
    (void)ConnectionHandle; /** @unused */
    (void)CompletionType; /** @unused */

    nRet = SQL_SUCCESS; /** no transactions */
 
    ODBCREST_PRINT( "SQLTransact(%p)->%d", EnvironmentHandle, nRet )

    return nRet;
}

ODBCREST_API 
SQLRETURN SQL_API SQLEndTran(
    SQLSMALLINT HandleType,
    SQLHANDLE Handle,
    SQLSMALLINT CompletionType) {

    SQLRETURN nRet = SQL_ERROR;

    (void)HandleType; /** @unused */
    (void)Handle; /** @unused */
    (void)CompletionType; /** @unused */

    nRet = SQL_SUCCESS; /** no transactions */
 
    ODBCREST_PRINT( "SQLEndTran(%d,%p)->%d", HandleType, Handle, nRet )

    return nRet;
}

