#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// structure to store the input
typedef struct {
    char *buffer;
    size_t buffer_length;
    size_t input_length;
} InputBuffer;

typedef enum { EXECUTE_SUCCESS, EXECUTE_TABLE_FULL } EXECUTE_RESULT;


// enum to represent the meta command result
typedef enum {
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZIED_COMMAND,
} META_COMMAND_RESULT;

// enum to represent the convert from string to command
typedef enum {
    PREPARE_SUCCESS,
    PREPARE_SYNTAX_ERROR,
    PREPARE_UNRECOGNIZIED_STATEMENT,
} PREPARE_RESULT;
// type about the statement
typedef enum { STATEMENT_INSERT, STATEMENT_SELECT } StatementType;

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE 255

typedef struct {
    uint32_t id;
    char username[COLUMN_USERNAME_SIZE];
    char email[COLUMN_EMAIL_SIZE];
} Row;

typedef struct {
    StatementType type;
    Row row_to_insert;// only used by insert
} Statement;

#define size_of_attribute(Struct, Attribute) sizeof(((Struct*)0) -> Attribute)

const uint32_t ID_SIZE = size_of_attribute(Row, id);
const uint32_t USERNAME_SIZE = size_of_attribute(Row, username);
const uint32_t EMAIL_SIZE = size_of_attribute(Row, email);
const uint32_t ID_OFFSET = 0;
const uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE;
const uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE;
const uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE;

const uint32_t PAGE_SIZE = 4096;
#define TABLE_MAX_PAGES 100
const uint32_t ROWS_PER_PAGE = PAGE_SIZE / ROW_SIZE;
const uint32_t TABLE_MAX_ROWS = ROWS_PER_PAGE * TABLE_MAX_PAGES;

typedef struct {
    uint32_t row_nums;
    void *pages[TABLE_MAX_PAGES];
} Table;

void print_row(Row* row) {
    printf("(%d, %s, %s)\n", row->id, row->username, row->email);
}

// change Row to raw bytes
void serialize_row(Row* source, void* destination) {
    // 受内存对齐影响容易错位、包含填充垃圾、缺乏跨平台兼容性
    // memcpy(destination, source, ROW_SIZE);
    memcpy((char*)destination + ID_OFFSET, &(source->id), ID_SIZE);
    memcpy((char*)destination + USERNAME_OFFSET, &(source->username), USERNAME_SIZE);
    memcpy((char*)destination + EMAIL_OFFSET, &(source->email), EMAIL_SIZE);
}

// change raw bytes to Row
void deserial_row(void* source, Row*destination){
    memcpy(&(destination->id), (char*)source+ID_OFFSET, ID_SIZE);
    memcpy(&(destination->username), (char*)source + USERNAME_OFFSET, USERNAME_SIZE);
    memcpy(&(destination->email), (char*)source + EMAIL_OFFSET, EMAIL_SIZE);
}

// locate the address of given row_num
void* row_slot(Table* table, uint32_t row_num) {
    uint32_t page_num = row_num / ROWS_PER_PAGE;
    void *page = table->pages[page_num];
    if(page==NULL) {
        page = table->pages[page_num] = malloc(PAGE_SIZE);
    }
    uint32_t row_offset = row_num % ROWS_PER_PAGE;
    uint32_t byte_offset = row_offset * ROW_SIZE;
    return (void*)((char*)page + byte_offset);
}
// create a new table
Table *new_table() { 
    Table *table = malloc(sizeof(Table));
    table->row_nums = 0;
    for (int i = 0; i < TABLE_MAX_PAGES;++i) {
        table->pages[i] = NULL;
    }
    return table;
}
// free table
void free_table(Table *table) { 
    for (int i = 0; i < TABLE_MAX_PAGES;++i) {
        free(table->pages[i]);
    }
    free(table);
}

// create InputBuffer
InputBuffer *new_input_buffer() {
    InputBuffer *new_buffer = malloc(sizeof(InputBuffer));
    new_buffer->buffer = NULL;
    new_buffer->buffer_length = 0;
    new_buffer->input_length = 0;
    return new_buffer;
}
// read input
void read_input(InputBuffer *input_buffer) {
    int read_bytes =
        getline(&input_buffer->buffer, &input_buffer->buffer_length, stdin);
    if(read_bytes<=0) {
        printf("[read_input ERROR]: fail to read stdin");
        exit(EXIT_FAILURE);
    }
    input_buffer->input_length = read_bytes - 1;
    input_buffer->buffer[read_bytes - 1] = 0;
}
// release the memory allocated to buffer
void close_input_buffer(InputBuffer *input_buffer) { 
    free(input_buffer->buffer);
    free(input_buffer);
}
// check if the meta command
META_COMMAND_RESULT do_meta_command(InputBuffer* input_buffer, Table*table) {
    if(strcmp(input_buffer->buffer, ".exit") == 0) {
        close_input_buffer(input_buffer);
        free_table(table);
        exit(EXIT_SUCCESS);
    } else {
        return META_COMMAND_UNRECOGNIZIED_COMMAND;
    }
}
// parse the statement to command
PREPARE_RESULT prepare_statement(InputBuffer* input_buffer, Statement* statement) {
    if(strncmp(input_buffer->buffer, "insert", 6)==0) {
        statement->type = STATEMENT_INSERT;
        int args_assigned = sscanf(input_buffer->buffer, "insert %d %s %s",
                                   (int *)&statement->row_to_insert.id,
                                   statement->row_to_insert.username,
                                   statement->row_to_insert.email);
        if(args_assigned<3) {
            return PREPARE_SYNTAX_ERROR;
        }
        return PREPARE_SUCCESS;
    } else if (strncmp(input_buffer->buffer, "select", 6)==0) {
        statement->type = STATEMENT_SELECT;
        return PREPARE_SUCCESS;
    } else {
        return PREPARE_UNRECOGNIZIED_STATEMENT;
    }
}
// database operator: insert
EXECUTE_RESULT execute_insert(Statement* statement, Table* table) {
    if(table->row_nums>=TABLE_MAX_ROWS) {
        return EXECUTE_TABLE_FULL;
    }
    Row *row_to_insert = &(statement->row_to_insert);
    serialize_row(row_to_insert, row_slot(table, table->row_nums));
    table->row_nums += 1;
    return EXECUTE_SUCCESS;
}
// database operator: select
EXECUTE_RESULT execute_select(__attribute__ ((unused)) Statement *statement, Table *table) { 
    Row row;
    for (int i = 0; i < (int)table->row_nums;++i) {
        deserial_row(row_slot(table, i), &row);
        print_row(&row);
    }
    return EXECUTE_SUCCESS;
}
// exec command
EXECUTE_RESULT execute_statement(Statement* statement, Table*table) {
    switch (statement->type) {
        case STATEMENT_INSERT:
            return execute_insert(statement, table);
        case STATEMENT_SELECT:
            return execute_select(statement, table);
        default:
            return EXECUTE_SUCCESS;
        }
}

int main() {
    Table *table = new_table();
    InputBuffer *input_buffer = new_input_buffer();
    while (true) {
        // 打印prompt提醒用户
        printf("db > ");
        // 读取输入
        read_input(input_buffer);
        // 解析输入
        if(input_buffer->buffer[0] == '.') {
            // meta command
            switch(do_meta_command(input_buffer, table)) {
                case (META_COMMAND_SUCCESS):
                    continue;
                case(META_COMMAND_UNRECOGNIZIED_COMMAND):
                    printf("unrecognized command '%s', try again!\n",
                           input_buffer->buffer);
                    continue;
            }
        }
        Statement statement;
        switch(prepare_statement(input_buffer, &statement)) {
            case PREPARE_SUCCESS:
                break;
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