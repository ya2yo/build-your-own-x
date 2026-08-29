#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// structure to store the input
typedef struct {
    char *buffer;
    size_t buffer_length;
    size_t input_length;
} InputBuffer;

// enum to represent the meta command result
typedef enum {
    META_COMMAND_SUCCESS,
    MEAT_COMMAND_UNRECOGNIZIED_COMMAND,
} META_COMMAND_RESULT;

// enum to represent the convert from string to command
typedef enum {
    PREPARE_SUCCESS,
    PREPARE_UNRECOGNIZIED_STATEMENT,
} PREPARE_RESULT;
// type about the statement
typedef enum { STATEMENT_INSERT, STATEMENT_SELECT } StatementType;
typedef struct {
    StatementType type;
} Statement;

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
void close_buffer(InputBuffer *input_buffer) { 
    free(input_buffer->buffer);
    free(input_buffer);
}
// check if the meta command
META_COMMAND_RESULT do_meta_command(InputBuffer* input_buffer) {
    if(strcmp(input_buffer->buffer, ".exit") == 0) {
        exit(EXIT_SUCCESS);
    } else {
        return MEAT_COMMAND_UNRECOGNIZIED_COMMAND;
    }
}
// parse the statement to command
PREPARE_RESULT prepare_statement(InputBuffer* input_buffer, Statement* statement) {
    if(strncmp(input_buffer->buffer, "insert", 6)==0) {
        printf("do insert");
        statement->type = STATEMENT_INSERT;
        return PREPARE_SUCCESS;
    } else if (strncmp(input_buffer->buffer, "select", 6)==0) {
        printf("do delete");
        statement->type = STATEMENT_SELECT;
        return PREPARE_SUCCESS;
    } else {
        printf("unrecognizied");
        return PREPARE_UNRECOGNIZIED_STATEMENT;
    }
}
// exec command
void execute_statement(Statement* statement) {
    switch (statement->type) {
        case STATEMENT_INSERT:
            /* code */
            break;
        case STATEMENT_SELECT:
            break;
        default:
            break;
    }
}

int main() {
    InputBuffer *input_buffer = new_input_buffer();
    while (true) {
        // 打印prompt提醒用户
        printf("db > ");
        // 读取输入
        read_input(input_buffer);
        // 解析输入
        if(input_buffer->buffer[0] == '.') {
            // meta command
            switch(do_meta_command(input_buffer)) {
                case (META_COMMAND_SUCCESS):
                    continue;
                case(MEAT_COMMAND_UNRECOGNIZIED_COMMAND):
                    printf("unrecognized command '%s', try again!\n",
                           input_buffer->buffer);
                    continue;
            }
        }
        Statement statement;
        switch(prepare_statement(input_buffer, &statement)) {
            case PREPARE_SUCCESS:
                break;
            case PREPARE_UNRECOGNIZIED_STATEMENT:
                printf("Unrecognized keyword at start of '%s'.\n",
                       input_buffer->buffer);
                continue;
        }
        execute_statement(&statement);
        printf("Executed.\n");
    }
}