# odbcrest

A lightweight, high-performance ODBC-like driver/interface built in C11 to bridge SQL applications with RESTL API data sources. This project allows developers to query variety data using standard SQL syntax. This ODBC driver is experimental and read-only. You can only execute queries.

## Features

- **SQL to API Translation:** Seamlessly translates relational SQL queries into REST requests.
- **Modern C11 Foundation:** Leverages modern C standards for optimal speed, performance, and memory safety.
- **Robust Networking:** Handles secure HTTPS requests authentication using `libcurl`.
- **JSON Processing:** High-speed parsing of complex API responses using `libjson-c`.

## Prerequisites & Dependencies

To build and run this project, you need a C11-compliant compiler (GCC or Clang) and the following developer libraries installed on your system:

- **C Compiler:** GCC 4.9+ or Clang 3.5+ (for C11 support)
- **libcurl:** Multiprotocol file transfer library (used for API communication)
- **libjson-c:** JSON parser object model library (used for payload processing)

### Architecture

Excel/App -> ODBC -> REST API -> Server/data

Main features:
* Data mapping
* Authentication
* Paging

### Installing Dependencies

#### Ubuntu / Debian
```bash
sudo apt update
sudo apt install build-essential libcurl4-openssl-dev libjson-c-dev unixodbc unixodbc-dev odbcinst
```

#### macOS (using Homebrew)
```bash
brew install curl json-c
```

#### Windows
```cmd
vcpkg install curl:x64-windows json-c:x64-windows
```

## Building the Project

The project uses a standard `CMakeLists.txt` for compilation. Clone the repository and compile using `cmake, make`:

```bash
# Clone the repository
git clone https://github.com/x/odbcrest
cd odbcrest

# Build the project
mkdir build
cd build
cmake ..
make
```

This will produce the shared library inside the `build/` directory: libodbcrest.so.

### Register the Driver (odbcinst.ini)

Open /etc/odbcinst.ini in a text editor:

```bash
sudo vi /etc/odbcinst.ini
```

Append the driver definition at the bottom: 

```ini
[MyDriver]
Description = ODBC Driver for REST API
Driver      = /path/to/your/odbcrest.so
Setup       = /path/to/your/odbcrest.so
```

### Configure the Data Source Name (odbc.ini)

Open your system-wide or user-specific configuration file:

```bash
vi ~/.odbc.ini
```

Add the DSN configuration block:

```ini
[MyDSN]
Driver      = MyDriver
Trace       = Yes
TraceFilev  = /tmp/odbcrest.log
```

### Test the Connection

You can verify that everything is configured correctly using the isql command-line utility: 

$ isql -v MyDSN


## Quick Start Example

Prepare endpoints and data mapping:

```ini
[table1]
name=users
url=https://api.github.com/users
header1=Accept: application/vnd.github+json
header2=User-Agent: odbcrest-driver/1.0
header3=Authorization: Bearer YOUR-TOKEN
jsonid1=login
jsonid2=id
```

Setup the environment:

$ export ODBCREST_INI=~/odbc_mapping.ini

Fetch data:

```isql
isql MyDSN
SQL> select * from users;
...

```

## Running Tests

To run the internal unit testing suite:

```bash
python3 odbcrest_tests.py
```

## Contributing

Contributions are highly welcome! Please feel free to submit a Pull Request or open an Issue for bug reports and feature requests. 

1. Fork the Project
2. Create your Feature Branch (`git checkout -b feature/AmazingFeature`)
3. Commit your Changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the Branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

## Alternatives

CData ODBC Driver for GitHub: Widely considered the gold standard for connecting non-database sources to enterprise tools. It provides a full SQL-92 layer over GitHub endpoints

Progress DataDirect for GitHub: A robust native-protocol driver built with a built-in SQL engine component.

PostgREST - PostgreSQL with RESTful API

## Notes

Since this project is experimental, multiple options can be trialed.

* Improve error handling
* Add full Unicode support
* Extend SQL queries
* Add column types
* Add column custom names
* Add column type conversions
* Add ODBC4 support

## License

Distributed under the MIT License. See `LICENSE` for more information.
