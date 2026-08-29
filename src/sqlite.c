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

// create InputBuffer
InputBuffer* new_input_buffer() {
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
// parse the input
void parse_command(char* command) {
    
}

int main() {
    InputBuffer *input_buffer = new_input_buffer();
    while (true) {
        // 打印prompt提醒用户
        printf("db > ");
        // 读取输入
        read_input(input_buffer);
        // 解析输入
        if(strcmp(input_buffer->buffer, "exit")){
            close_buffer(input_buffer);
            exit(EXIT_SUCCESS);
        } else {
            printf("unknown command");
            exit(EXIT_FAILURE);
        }
    }
}