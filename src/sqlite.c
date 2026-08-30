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
    void *pages[TABLE_MAX_PAGES];
} Pager;

/* ================================ Table ================================= */

/** Represents an open table and its associated pager. */
typedef struct {
    uint32_t row_nums;
    Pager *pager;
} Table;

/* Serialized row layout.  These offsets deliberately avoid struct padding. */
const uint32_t ID_SIZE = size_of_attribute(Row, id);
const uint32_t USERNAME_SIZE = size_of_attribute(Row, username);
const uint32_t EMAIL_SIZE = size_of_attribute(Row, email);
const uint32_t ID_OFFSET = 0;
const uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE;
const uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE;

const uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE;
const uint32_t PAGE_SIZE = 4096;
const uint32_t ROWS_PER_PAGE = PAGE_SIZE / ROW_SIZE;
const uint32_t TABLE_MAX_ROWS = ROWS_PER_PAGE * TABLE_MAX_PAGES;

const uint32_t TABLE_SIZE = sizeof(Table);

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

/* ================================ Pager ================================= */

/** Opens the database file and initializes its pager and page cache. */
Pager *pager_open(const char *filename) {}

/** Returns the requested page, loading it into the pager cache if necessary. */
void* get_page(Pager* pager, uint32_t page_num) {
    if(page_num > TABLE_MAX_PAGES) {
        printf("Tried to fetch page number out of bounds. %d > %d.\n", page_num,
               TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }
}

/** Writes a page from the in-memory cache back to the database file. */
void page_flush(Pager *pager, uint32_t page_num, uint32_t size) {}

/* ================================ Table ================================= */

/** Opens a table and derives its current row count from the file length. */
Table* db_open(const char* filename) {
    Pager* pager = pager_open(filename);
    uint32_t rows = pager->file_length / ROW_SIZE;

    Table *table = malloc(TABLE_SIZE);
    table->pager = pager;
    table->row_nums = rows;
    return table;
}

/** Returns the address of the serialized slot for the given row number. */
void* row_slot(Table* table, uint32_t row_num) {
    uint32_t page_num = row_num / ROWS_PER_PAGE;
    void *page = get_page(table->pager, page_num);
    uint32_t row_offset = row_num % ROWS_PER_PAGE;
    uint32_t byte_offset = row_offset * ROW_SIZE;
    return (void*)((char*)page + byte_offset);
}

/** Flushes cached pages and releases all resources owned by the table. */
void db_close(Table* table) {

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
    if(table->row_nums>=TABLE_MAX_ROWS) {
        return EXECUTE_TABLE_FULL;
    }
    Row *row_to_insert = &(statement->row_to_insert);
    serialize_row(row_to_insert, row_slot(table, table->row_nums));
    table->row_nums += 1;
    return EXECUTE_SUCCESS;
}
/** Executes a SELECT statement by printing every stored row. */
EXECUTE_RESULT execute_select(__attribute__ ((unused)) Statement *statement,
                              Table *table) {
    Row row;
    for (uint32_t i = 0; i < table->row_nums; ++i) {
        deserial_row(row_slot(table, i), &row);
        print_row(&row);
    }
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