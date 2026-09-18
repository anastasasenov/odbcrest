# --------------
# odbcrest tests
#
#   pip install pyodbc asyncio aiohttp
# --------------

import os
import pyodbc
import asyncio
import threading
import time
from aiohttp import web

# setup DNS
dsn_name = "odbcrest"
connection_string = f"DSN={dsn_name};"
ini_file = "/tmp/odbcrest_test.ini"
env_var = "ODBCREST_INI"
HOST = "127.0.0.1"
PORT = 8083

def setup():
    os.environ[ env_var ] = ini_file
    f = open(ini_file, "w")
    f.write("[table1]\n")
    f.write("name=tbl\n")
    f.write(f"url=http://{HOST}:{PORT}/data\n")
    f.write("jsonid1=col1\n")
    f.write("jsonid2=col2\n")
    f.write("jsonid3=col3\n")
    f.close()
   
def open_connection(connection_string):
    conn = pyodbc.connect(connection_string)
    conn.setencoding(encoding="utf-8")
    conn.setdecoding(pyodbc.SQL_CHAR, encoding="utf-8")
    conn.setdecoding(pyodbc.SQL_WCHAR, encoding="utf-8")
    conn.setdecoding(pyodbc.SQL_WMETADATA, encoding="utf-8")
    return conn

def close_connection(conn):
    conn.close()

def get_odbc_error(ex):
    sqlstate = ex.args[0]
    ret = f"\n[ODBC Error]: {ex} SQLSTATE: {sqlstate}"
    return ret

async def handle_data(request):
    data = [
        {"col1": "1", "col2": "watch", "col3": "150.00"},
        {"col1": "2", "col2": "laptop", "col3": "1050.00"}
    ]
    return web.json_response(data)

def start_rest_srv(host, port):
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    app = web.Application()
    app.router.add_get('/data', handle_data)
    
    runner = web.AppRunner(app)
    loop.run_until_complete(runner.setup())
    
    site = web.TCPSite(runner, host, port)
    loop.run_until_complete(site.start())
    try:
        print(f"lannch local rest-serv http://{host}:{port}")
        loop.run_forever()
    finally:
        loop.run_until_complete(runner.cleanup())
        loop.close()

def test1():
    print("Test 1: connect/disconnet ...", end=' ')
    try:
        conn = open_connection(connection_string)
        print("OK")
    except pyodbc.Error as ex:
        assert False, f"Connection failed: {get_odbc_error(ex)}"
    finally:
        close_connection(conn)

def test2():
    print("Test 2: tables {", end=' ')
    try:
        conn = open_connection(connection_string)
        cursor = conn.cursor()
        nNumOfTbl = 0
        for row in cursor.tables():
            table_name = row[2]
            print(f"{table_name}", end=" ")
            nNumOfTbl = nNumOfTbl + 1
        cursor.close()
        assert nNumOfTbl > 0, "No info about tables"
        print("} ... OK")
    except pyodbc.Error as ex:
        assert False, f"Connection failed: {get_odbc_error(ex)}"
    finally:
        close_connection(conn)

def test3():
    print("Test 3: columns {", end=' ')
    try:
        conn = open_connection(connection_string)
        cursor = conn.cursor()
        nNumOfCol = 0
        for row in cursor.columns():
            table_name = row[2]
            column_name = row[3]
            print(f"{table_name}.{column_name}", end=" ")
            nNumOfCol = nNumOfCol + 1
        cursor.close()
        assert nNumOfCol > 0, "No info about columns"
        print("} ... OK")
    except pyodbc.Error as ex:
        assert False, f"Connection failed: {get_odbc_error(ex)}"
    finally:
        close_connection(conn)
        
def test4():
    print("Test 4: read table 'tbl' ...", end = ' ')
    try:
        conn = open_connection(connection_string)
        cursor = conn.cursor()
        query = f"SELECT * FROM tbl;" 
        cursor.execute(query)
        rows = cursor.fetchall()      
        if not rows:
            assert False, "The table is empty"
        else:
            print("{", end = ' ')
            for row in rows:
                print(row, end = ' ')
            print("} ... OK")
    except pyodbc.Error as ex:
        assert False, f"Connection failed: {get_odbc_error(ex)}"
    finally:
        close_connection(conn)

if __name__ == "__main__":
    api_thread = threading.Thread(target=start_rest_srv, args=(HOST, PORT), daemon=True)
    api_thread.start()
    time.sleep(1)
    setup()
    test1()
    test2()
    test3()
    test4()

