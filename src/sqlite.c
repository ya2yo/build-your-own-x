#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define size_of_attribute(Struct, Attribute) sizeof(((Struct *)0)->Attribute)
#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE 255
#define TABLE_MAX_PAGES 100

/** Represents the type of node */
typedef enum {
    NODE_INTERNAL,
    NODE_LEAF,
} NodeType;

/** Stores one line of input read from standard input. */
typedef struct {
    char *buffer;
    size_t buffer_length;
    size_t input_length;
} InputBuffer;

/** Results that can be returned while executing a prepared statement. */
typedef enum { EXECUTE_SUCCESS, EXECUTE_TABLE_FULL } EXECUTE_RESULT;

/** Results returned while handling a dot-prefixed meta-command. */
typedef enum {
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZIED_COMMAND,
} META_COMMAND_RESULT;

/** Results returned while converting user input into a statement. */
typedef enum {
    PREPARE_SUCCESS,
    PREPARE_NEGATIVE_ID,
    PREPARE_STRING_TOO_LONG,
    PREPARE_SYNTAX_ERROR,
    PREPARE_UNRECOGNIZIED_STATEMENT,
} PREPARE_RESULT;

/** Represents one row stored by the database. */
typedef struct {
    uint32_t id;
    char username[COLUMN_USERNAME_SIZE + 1];
    char email[COLUMN_EMAIL_SIZE + 1];
} Row;

/** Identifies the SQL-like statement being executed. */
typedef enum { STATEMENT_INSERT, STATEMENT_SELECT } StatementType;

/** A parsed statement and the row data required by INSERT. */
typedef struct {
    StatementType type;
    Row row_to_insert; /* Used only for INSERT statements. */
} Statement;

/* ================================ Pager ================================= */

/** Owns the file descriptor and in-memory page cache for a database file. */
typedef struct {
    int fd;
    uint32_t file_length;
    uint32_t num_pages;
    void *pages[TABLE_MAX_PAGES];
} Pager;

/* ================================ Table ================================= */

/** Represents an open table and its associated pager. */
typedef struct {
    uint32_t root_num_page;
    Pager *pager;
} Table;

/* ================================ Cursor ================================= */

/** Repesents a loaction in the table */
typedef struct {
    Table *table;
    uint32_t page_num;
    uint32_t cell_num;
    bool end_of_table; // Indicates a position one past the last element
} Cursor;

/* Serialized row layout.  These offsets deliberately avoid struct padding. */
const uint32_t ID_SIZE = size_of_attribute(Row, id);
const uint32_t USERNAME_SIZE = size_of_attribute(Row, username);
const uint32_t EMAIL_SIZE = size_of_attribute(Row, email);
const uint32_t ID_OFFSET = 0;
const uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE;
const uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE;

const uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE;
const uint32_t PAGE_SIZE = 4096;

const uint32_t PAGER_SIZE = sizeof(Pager);
const uint32_t TABLE_SIZE = sizeof(Table);
const uint32_t CURSOR_SIZE = sizeof(Cursor);

/* Common Node Header Layout */
const uint32_t NODE_TYPE_SIZE = sizeof(uint8_t);
const uint32_t NODE_TYPE_OFFSET = 0;
const uint32_t IS_ROOT_SIZE = sizeof(uint8_t);
const uint32_t IS_ROOT_OFFSET = NODE_TYPE_SIZE;
const uint32_t PARENT_POINTER_SIZE = sizeof(uint32_t);
const uint32_t PARENT_POINTER_OFFSET = IS_ROOT_SIZE + IS_ROOT_OFFSET;
const uint32_t COMMON_NODE_HEADER_SIZE =
    NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE;

/* Leaf Node Header layout */
const uint32_t LEAF_NODE_NUM_CELLS_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_HEADER_SIZE =
    COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE;

/* Lead Node Body Layout */
const uint32_t LEAF_NODE_KEY_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_KEY_OFFSET = 0;
const uint32_t LEAF_NODE_VALUE_SIZE = ROW_SIZE;
const uint32_t LEAF_NODE_VALUE_OFFSET =
    LEAF_NODE_KEY_SIZE + LEAF_NODE_KEY_OFFSET;
const uint32_t LEAF_NODE_CELL_SIZE = LEAF_NODE_KEY_SIZE + LEAF_NODE_VALUE_SIZE;
const uint32_t LEAF_NODE_SPACE_FOR_CELLS = PAGE_SIZE - LEAF_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_MAX_CELLS =
    LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE;

/* ================== Function Declaration ========================= */

/* ================== Print Function =============================*/
void print_row(Row *row);
void print_constants();
void print_leaf_node(void *node);

void serialize_row(Row *source, void *destination);
void deserial_row(void *source, Row *destination);

void *get_page(Pager *pager, uint32_t page_num);

/** Prints a row in the format used by the interactive shell. */
void print_row(Row* row) {
    printf("(%d, %s, %s)\n", row->id, row->username, row->email);
}

/** Serializes a row into the fixed-width byte layout used on disk.
 *
 * Copying each field separately prevents compiler-inserted struct padding from
 * becoming part of the file format and keeps the serialized layout explicit.
 */
void serialize_row(Row* source, void* destination) {
    memcpy((char*)destination + ID_OFFSET, &(source->id), ID_SIZE);
    memcpy((char*)destination + USERNAME_OFFSET, &(source->username), USERNAME_SIZE);
    memcpy((char*)destination + EMAIL_OFFSET, &(source->email), EMAIL_SIZE);
}

/** Reconstructs a row from its fixed-width serialized representation. */
void deserial_row(void* source, Row* destination) {
    memcpy(&(destination->id), (char*)source + ID_OFFSET, ID_SIZE);
    memcpy(&(destination->username), (char*)source + USERNAME_OFFSET,
           USERNAME_SIZE);
    memcpy(&(destination->email), (char*)source + EMAIL_OFFSET, EMAIL_SIZE);
}

/* ================================ Node  ================================= */

/** Return the address of the num_cells in the node page header */
uint32_t* leaf_node_num_cells(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_NUM_CELLS_OFFSET);
}

/** Return the cell which num is cell_num int the node page nody */
void* leaf_node_cell(void*node, uint32_t cell_num) {
    return (char*)node + LEAF_NODE_HEADER_SIZE + cell_num * LEAF_NODE_CELL_SIZE;
}

/* Return a pointer to the key of the given cell in a leaf node */
uint32_t* leaf_node_key(void*node, uint32_t cell_num) {
    return (uint32_t*)((char*)leaf_node_cell(node, cell_num) + LEAF_NODE_KEY_OFFSET);
}

/* Return a pointer to the value of the given cell in a leaf node*/
void* leaf_node_value(void* node, uint32_t cell_num) {
    return (char*)leaf_node_cell(node, cell_num) + LEAF_NODE_VALUE_OFFSET;
}

/* Initialize the given node*/
void initialize_leaf_node(void *node) { *leaf_node_num_cells(node) = 0; }

/* Insert a key/value pair into a leaf node*/
void leaf_node_insert(Cursor* cursor, uint32_t key, Row* value) {
    void *node = get_page(cursor->table->pager, cursor->page_num);

    uint32_t num_cells = *leaf_node_num_cells(node);
    if(num_cells>=LEAF_NODE_MAX_CELLS) {
        printf("Need to implement splitting a leaf node.\n");
        exit(EXIT_FAILURE);
    }

    if(cursor->cell_num<num_cells) {
        // Make room for new cell
        for (uint32_t i = num_cells; i > cursor->cell_num;i--) {
            memcpy(leaf_node_cell(node, i), leaf_node_cell(node, i-1),
                   LEAF_NODE_CELL_SIZE);
        }
    }

    *(leaf_node_num_cells(node)) += 1;
    *(leaf_node_key(node, cursor->cell_num)) = key;
    serialize_row(value, leaf_node_value(node, cursor->cell_num));
}

void print_constants() {
    printf("ROW_SIZE: %d\n", ROW_SIZE);
    printf("COMMON_NODE_HEADER_SIZE: %d\n", COMMON_NODE_HEADER_SIZE);
    printf("LEAF_NODE_HEADER_SIZE: %d\n", LEAF_NODE_HEADER_SIZE);
    printf("LEAF_NODE_CELL_SIZE: %d\n", LEAF_NODE_CELL_SIZE);
    printf("LEAF_NODE_SPACE_FOR_CELLS: %d\n", LEAF_NODE_SPACE_FOR_CELLS);
    printf("LEAF_NODE_MAX_CELLS: %d\n", LEAF_NODE_MAX_CELLS);
}

void print_leaf_node(void *node) {
    uint32_t num_cells = *leaf_node_num_cells(node);
    printf("leaf (size %d)\n", num_cells);
    for (uint32_t i = 0; i < num_cells; i++) {
        uint32_t key = *leaf_node_key(node, i);
        printf("  - %d : %d\n", i, key);
        
    }
}

/* ================================ Pager ================================= */

/** Opens the database file and initializes its pager and page cache. */
Pager *pager_open(const char *filename) {
    int fd = open(filename, O_CREAT | O_RDWR, S_IWUSR | S_IRUSR);
    if(fd == -1) {
        printf("Unable to open %s", filename);
        exit(EXIT_FAILURE);
    }

    off_t file_length = lseek(fd, 0, SEEK_END);

    Pager *pager = malloc(PAGER_SIZE);
    pager->fd = fd;
    pager->file_length = file_length;
    pager->num_pages = file_length / PAGE_SIZE;

    if(file_length % PAGE_SIZE != 0) {
        printf("Db file is not a whole number of page. Corrupt file.\n");
        exit(EXIT_FAILURE);
    }

    for (uint32_t i = 0; i < TABLE_MAX_PAGES;++i) {
        pager->pages[i] = NULL;
    }
    return pager;
}

/** Returns the requested page, loading it into the pager cache if necessary. */
void* get_page(Pager* pager, uint32_t page_num) {
    if(page_num >= TABLE_MAX_PAGES) {
        printf("Tried to fetch page number out of bounds. %d >= %d.\n", page_num,
               TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }
    if(pager->pages[page_num]==NULL) {
        // Cache miss. Allocate memory and load from file.
        void *page = malloc(PAGE_SIZE);
        uint32_t num_pages = (pager->file_length + PAGE_SIZE - 1) / PAGE_SIZE;

        if (page_num < num_pages) {
            // the data have been existed
            lseek(pager->fd, page_num * PAGE_SIZE, SEEK_SET);
            ssize_t bytes_read = read(pager->fd, page, PAGE_SIZE);
            if (bytes_read == -1) {
                printf("Error reading file: %d\n", errno);
                exit(EXIT_FAILURE);
            }
        }
        pager->pages[page_num] = page;

        if(page_num >= pager->num_pages) {
            pager->num_pages = page_num + 1;
        }
    }
    return pager->pages[page_num];
}

/** Writes a page from the in-memory cache back to the database file. */
void page_flush(Pager *pager, uint32_t page_num) {
    if(page_num >= TABLE_MAX_PAGES) {
        printf("Tried to write page number out of bounds. %d >= %d.\n",
               page_num, TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }
    if(pager->pages[page_num] == NULL) {
        printf("Tried to flush null page.\n");
        exit(EXIT_FAILURE);
    }

    off_t offset = lseek(pager->fd, page_num * PAGE_SIZE, SEEK_SET);

    if (offset == -1) {
        printf("Error seeking: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    ssize_t bytes_written = write(pager->fd, pager->pages[page_num], PAGE_SIZE);
    if(bytes_written==-1) {
        printf("Error writing file: %d\n", errno);
        exit(EXIT_FAILURE);
    }
}

/* ================================ Table ================================= */

/** Opens a table and derives its current row count from the file length. */
Table* db_open(const char* filename) {
    Pager* pager = pager_open(filename);

    Table *table = malloc(TABLE_SIZE);
    table->pager = pager;
    table->root_num_page = 0;

    if(pager->num_pages == 0) {
        // New database file. Initialize page 0 as leaf node.
        void *root_node = get_page(pager, 0);
        initialize_leaf_node(root_node);
    }
    return table;
}

/** Flushes cached pages and releases all resources owned by the table. */
void db_close(Table* table) {
    Pager* pager = table->pager;

    for (uint32_t i = 0; i < pager->num_pages;++i) {
        if(pager->pages[i]==NULL) {
            continue;
        }
        page_flush(pager, i);
        free(pager->pages[i]);
        pager->pages[i] = NULL;
    }

    int result = close(pager->fd);
    if(result==-1) {
        printf("Error closing db file.\n");
        exit(EXIT_FAILURE);
    }
    
    // defendent code. Maybe could delete it
    for (uint32_t i = 0; i < TABLE_MAX_PAGES;++i) {
        void *page = pager->pages[i];
        if(page) {
            free(page);
            pager->pages[i] = NULL;
        }
    }

    free(pager);
    free(table);
}

/* =============================== Cursor ================================== */

/** create a Cursor object in the start of table */
Cursor* table_start(Table *table) {
    Cursor* cursor = malloc(CURSOR_SIZE);
    cursor->table = table;
    cursor->page_num = table->root_num_page;
    cursor->cell_num = 0;

    void *root_node = get_page(table->pager, table->root_num_page);
    uint32_t num_cells = *leaf_node_num_cells(root_node);
    cursor->end_of_table = (num_cells == 0);

    return cursor;
}

/** create a Cursor object in the end of table */
Cursor* table_end(Table* table) {
    Cursor* cursor = malloc(CURSOR_SIZE);
    cursor->table = table;
    cursor->page_num = table->root_num_page;

    void *root_node = get_page(table->pager, table->root_num_page);
    uint32_t num_cells = *leaf_node_num_cells(root_node);
    cursor->cell_num = num_cells;
    cursor->end_of_table = true;

    return cursor;
}

/** return a pointer to the position descibed by the cursor */
void* cursor_value(Cursor* cursor) {
    uint32_t page_num = cursor->page_num;
    void *page = get_page(cursor->table->pager, page_num);
    return leaf_node_value(page, cursor->cell_num);
}

/** advance the cursor in the table */
void cursor_advance(Cursor* cursor) {
    uint32_t page_num = cursor->page_num;
    void *node = get_page(cursor->table->pager, page_num);

    uint32_t cell_num = *leaf_node_num_cells(node);
    cursor->cell_num += 1;
    if(cursor->cell_num>=cell_num) {
        cursor->end_of_table = true;
    }
}

/* ============================= Input buffer ============================== */

/** Allocates and initializes an empty input buffer. */
InputBuffer *new_input_buffer() {
    InputBuffer *new_buffer = malloc(sizeof(InputBuffer));
    new_buffer->buffer = NULL;
    new_buffer->buffer_length = 0;
    new_buffer->input_length = 0;
    return new_buffer;
}

/** Reads one line from standard input and removes its trailing newline. */
void read_input(InputBuffer *input_buffer) {
    int read_bytes =
        getline(&input_buffer->buffer, &input_buffer->buffer_length, stdin);
    if(read_bytes <= 0) {
        printf("[read_input ERROR]: fail to read stdin");
        exit(EXIT_FAILURE);
    }
    input_buffer->input_length = read_bytes - 1;
    input_buffer->buffer[read_bytes - 1] = 0;
}

/** Releases the buffer and the InputBuffer object itself. */
void close_input_buffer(InputBuffer *input_buffer) {
    free(input_buffer->buffer);
    free(input_buffer);
}

/** Executes a dot-prefixed command such as `.exit`. */
META_COMMAND_RESULT do_meta_command(InputBuffer* input_buffer, Table* table) {
    if(strcmp(input_buffer->buffer, ".exit") == 0) {
        close_input_buffer(input_buffer);
        db_close(table);
        exit(EXIT_SUCCESS);
    } else if(strcmp(input_buffer->buffer, ".btree") == 0) {
        printf("Tree: \n");
        print_leaf_node(get_page(table->pager, 0));
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input_buffer->buffer, ".constants") == 0) {
        printf("Constants: \n");
        print_constants();
        return META_COMMAND_SUCCESS;
    } else {
        return META_COMMAND_UNRECOGNIZIED_COMMAND;
    }
}

/** Parses the arguments of an INSERT statement into a Statement object. */
PREPARE_RESULT prepare_insert(InputBuffer* input_buffer, Statement* statement) {
    statement->type = STATEMENT_INSERT;
    strtok(input_buffer->buffer, " ");
    char *id_string = strtok(NULL, " ");
    char *username = strtok(NULL, " ");
    char *email = strtok(NULL, " ");

    if(id_string==NULL||username==NULL||email==NULL) {
        return PREPARE_SYNTAX_ERROR;
    }

    int id = atoi(id_string);
    if(id<0) {
        return PREPARE_NEGATIVE_ID;
    }
    if(strlen(username)>COLUMN_USERNAME_SIZE) {
        return PREPARE_STRING_TOO_LONG;
    }
    if(strlen(email)>COLUMN_EMAIL_SIZE) {
        return PREPARE_STRING_TOO_LONG;
    }
    statement->row_to_insert.id = id;
    strcpy(statement->row_to_insert.username, username);
    strcpy(statement->row_to_insert.email, email);

    return PREPARE_SUCCESS;
}
/** Converts the input buffer into a prepared statement. */
PREPARE_RESULT prepare_statement(InputBuffer* input_buffer, Statement* statement) {
    if(strncmp(input_buffer->buffer, "insert", 6)==0) {
        return prepare_insert(input_buffer, statement);
    } else if (strncmp(input_buffer->buffer, "select", 6) == 0) {
        statement->type = STATEMENT_SELECT;
        return PREPARE_SUCCESS;
    } else {
        return PREPARE_UNRECOGNIZIED_STATEMENT;
    }
}
/** Executes an INSERT statement and stores the new row in its slot. */
EXECUTE_RESULT execute_insert(Statement* statement, Table* table) {
    void *node = get_page(table->pager, table->root_num_page);
    if(*(leaf_node_num_cells(node)) >= LEAF_NODE_MAX_CELLS) {
        return EXECUTE_TABLE_FULL;
    }

    Row *row_to_insert = &(statement->row_to_insert);
    Cursor *cursor = table_end(table);
    leaf_node_insert(cursor, row_to_insert->id, row_to_insert);
    // table->row_nums += 1;
    free(cursor);
    return EXECUTE_SUCCESS;
}
/** Executes a SELECT statement by printing every stored row. */
EXECUTE_RESULT execute_select(__attribute__ ((unused)) Statement *statement,
                              Table *table) {
    Row row;
    Cursor *cursor = table_start(table);
    while(!cursor->end_of_table) {
        deserial_row(cursor_value(cursor), &row);
        print_row(&row);
        cursor_advance(cursor);
    }
    free(cursor);
    return EXECUTE_SUCCESS;
}
/** Dispatches a prepared statement to its execution function. */
EXECUTE_RESULT execute_statement(Statement* statement, Table* table) {
    switch (statement->type) {
        case STATEMENT_INSERT:
            return execute_insert(statement, table);
        case STATEMENT_SELECT:
            return execute_select(statement, table);
        default:
            return EXECUTE_SUCCESS;
        }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Must supply a database filename.\n");
        exit(EXIT_FAILURE);
    }
    char *filename = argv[1];
    Table *table = db_open(filename);

    InputBuffer *input_buffer = new_input_buffer();
    while (true) {
        /* Display the prompt and read one command from the user. */
        printf("db > ");
        read_input(input_buffer);
        if(input_buffer->buffer[0] == '.') {
            /* Dot-prefixed commands are handled separately from statements. */
            switch(do_meta_command(input_buffer, table)) {
                case META_COMMAND_SUCCESS:
                    continue;
                case META_COMMAND_UNRECOGNIZIED_COMMAND:
                    printf("unrecognized command '%s', try again!\n",
                           input_buffer->buffer);
                    continue;
            }
        }
        Statement statement;
        /* Parse the command, then execute it if parsing succeeds. */
        switch(prepare_statement(input_buffer, &statement)) {
            case PREPARE_SUCCESS:
                break;
            case PREPARE_NEGATIVE_ID:
                printf("ID must be positive.\n");
                continue;
            case PREPARE_STRING_TOO_LONG:
                printf("String is too long.\n");
                continue;
            case PREPARE_SYNTAX_ERROR:
                printf("Syntax error. Could not parse statement.\n");
                continue;
            case PREPARE_UNRECOGNIZIED_STATEMENT:
                printf("Unrecognized keyword at start of '%s'.\n",
                       input_buffer->buffer);
                continue;
        }
        switch(execute_statement(&statement, table)) {
            case EXECUTE_SUCCESS:
                printf("Executed.\n");
                break;
            case EXECUTE_TABLE_FULL:
                printf("Error: Table full.\n");
                break;
        }
    }
}