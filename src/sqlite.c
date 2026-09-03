/**
 * # The leaf_node layout
 * | byte 0     | byte 1        | bytes 2-5         | bytes 6-9 |
 * | node_type  | is_root       | parent_pointer    | num_cells |
 * |bytes 6-9   | bytes 10-13   | bytes 14-306      |
 * | num_cells  | key 0         | value 0           |
 */

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
#define TABLE_MAX_PAGES 400
#define INVALID_PAGE_NUM UINT32_MAX

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
typedef enum {
    EXECUTE_SUCCESS,
    EXECUTE_DUPLICATE_KEY,
    EXECUTE_TABLE_FULL
} EXECUTE_RESULT;

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
const uint32_t LEAF_NODE_NEXT_LEAF_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_NEXT_LEAF_OFFSET =
    LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE;
const uint32_t LEAF_NODE_HEADER_SIZE =
    COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE +
    LEAF_NODE_NEXT_LEAF_SIZE;

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

/* Leaf Node Split Strategy */
const uint32_t LEAF_NODE_RIGHT_SPLIT_COUNT = (LEAF_NODE_MAX_CELLS + 1) / 2;
const uint32_t LEAF_NODE_LEFT_SPLIT_COUNT =
    LEAF_NODE_MAX_CELLS + 1 - LEAF_NODE_RIGHT_SPLIT_COUNT;

/* Internal Node Header Layout */
const uint32_t INTERNAL_NODE_NUM_KEYS_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_NUM_KEYS_OFFSET = COMMON_NODE_HEADER_SIZE;
const uint32_t INTERNAL_NODE_RIGHT_CHILD_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_RIGHT_CHILD_OFFSET =
    INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE;
const uint32_t INTERNAL_NODE_HEADER_SIZE =
    COMMON_NODE_HEADER_SIZE + INTERNAL_NODE_NUM_KEYS_SIZE +
    INTERNAL_NODE_RIGHT_CHILD_SIZE;

/* Internal Node Body Layout */
const uint32_t INTERNAL_NODE_KEY_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_CHILD_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_CELL_SIZE =
    INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE;
/* Keep the fanout small so the tutorial's internal-node split path is easy
 * to observe with a modest number of rows. */
const uint32_t INTERNAL_NODE_MAX_KEYS = 3;

/* ================== Function Declarations ========================= */

/* Row and display */
void print_row(Row *row);
void print_constants(void);
void print_leaf_node(void *node);
void serialize_row(Row *source, void *destination);
void deserial_row(void *source, Row *destination);

/* Node type*/
NodeType get_node_type(void *node);
void set_node_type(void *node, NodeType type);
bool is_node_root(void *node);
void set_node_root(void *node, bool is_root);
uint32_t *node_parent(void *node);

/* Node */
uint32_t *internal_node_num_keys(void *node);
uint32_t *internal_node_right_child(void *node);
uint32_t *internal_node_cell(void *node, uint32_t cell_num);
uint32_t *internal_node_child(void *node, uint32_t child_num);
uint32_t *internal_node_key(void *node, uint32_t key_num);
uint32_t get_node_max_key(Pager *pager, void *node);
uint32_t internal_node_find_child(void *node, uint32_t key);
Cursor *internal_node_find(Table *table, uint32_t page_num, uint32_t key);
void initialize_internal_node(void *node);
void internal_node_insert(Table *table, uint32_t parent_page_num,
                          uint32_t child_page_num);
void internal_node_split_and_insert(Table *table, uint32_t parent_page_num,
                                    uint32_t child_page_num);
void update_internal_node_key(void *node, uint32_t old_key, uint32_t new_key);
uint32_t *leaf_node_num_cells(void *node);
uint32_t *leaf_node_next_leaf(void *node);
void *leaf_node_cell(void *node, uint32_t cell_num);
uint32_t *leaf_node_key(void *node, uint32_t cell_num);
void *leaf_node_value(void *node, uint32_t cell_num);
void initialize_leaf_node(void *node);
void leaf_node_insert(Cursor *cursor, uint32_t key, Row *value);
void leaf_node_split_and_insert(Cursor *cursor, uint32_t key, Row *value);
Cursor *leaf_node_find(Table *table, uint32_t page_num, uint32_t key);

void create_new_root(Table *table, uint32_t right_child_page_num);
void print_tree(Pager *pager, uint32_t page_num, uint32_t indentation_level);

/* Pager */
Pager *pager_open(const char *filename);
void *get_page(Pager *pager, uint32_t page_num);
void page_flush(Pager *pager, uint32_t page_num);
uint32_t get_unused_page_num(Pager *pager);

/* Table */
Table *db_open(const char *filename);
void db_close(Table *table);

/* Cursor */
Cursor *table_start(Table *table);
Cursor *table_find(Table *table, uint32_t key);
void *cursor_value(Cursor *cursor);
void cursor_advance(Cursor *cursor);

/* Input buffer */
InputBuffer *new_input_buffer(void);
void read_input(InputBuffer *input_buffer);
void close_input_buffer(InputBuffer *input_buffer);

/* Meta commands, parsing, and execution */
META_COMMAND_RESULT do_meta_command(InputBuffer *input_buffer, Table *table);
PREPARE_RESULT prepare_insert(InputBuffer *input_buffer, Statement *statement);
PREPARE_RESULT prepare_statement(InputBuffer *input_buffer, Statement *statement);
EXECUTE_RESULT execute_insert(Statement *statement, Table *table);
EXECUTE_RESULT execute_select(Statement *statement, Table *table);
EXECUTE_RESULT execute_statement(Statement *statement, Table *table);

/* ================================ Main ================================== */

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
            case EXECUTE_DUPLICATE_KEY:
                printf("Error: Duplicate key.\n");
                break;
            case EXECUTE_TABLE_FULL:
                printf("Error: Table full.\n");
                break;
        }
    }
}

/* ================================ Print / Serialization ================= */

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

/* =============================== Node Type ============================== */

/* Return the tyoe of given node */
NodeType get_node_type(void *node) {
    uint8_t value = *((uint8_t *)((char *)node + NODE_TYPE_OFFSET));
    return (NodeType)value;
}

void set_node_type(void *node, NodeType type) {
    uint8_t value = type;
    *((uint8_t *)((char *)node + NODE_TYPE_OFFSET)) = value;
}

bool is_node_root(void *node) {
    return *((uint8_t *)((char *)node + IS_ROOT_OFFSET)) != 0;
}

void set_node_root(void *node, bool is_root) {
    *((uint8_t *)((char *)node + IS_ROOT_OFFSET)) = is_root ? 1 : 0;
}

uint32_t *node_parent(void *node) {
    return (uint32_t *)((char *)node + PARENT_POINTER_OFFSET);
}

/* ================================ Node ================================== */

uint32_t *internal_node_num_keys(void *node) {
    return (uint32_t *)((char *)node + INTERNAL_NODE_NUM_KEYS_OFFSET);
}

uint32_t *internal_node_right_child(void *node) {
    return (uint32_t *)((char *)node + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}

uint32_t *internal_node_cell(void *node, uint32_t cell_num) {
    return (uint32_t *)((char *)node + INTERNAL_NODE_HEADER_SIZE +
                        cell_num * INTERNAL_NODE_CELL_SIZE);
}

uint32_t *internal_node_child(void *node, uint32_t child_num) {
    uint32_t num_keys = *internal_node_num_keys(node);
    if (child_num > num_keys) {
        fprintf(stderr, "Tried to access child %u > num_keys %u.\n",
                child_num, num_keys);
        exit(EXIT_FAILURE);
    }
    if (child_num == num_keys) {
        uint32_t *right_child = internal_node_right_child(node);
        if (*right_child == INVALID_PAGE_NUM) {
            fprintf(stderr, "Tried to access an invalid right child.\n");
            exit(EXIT_FAILURE);
        }
        return right_child;
    }
    uint32_t *child = internal_node_cell(node, child_num);
    if (*child == INVALID_PAGE_NUM) {
        fprintf(stderr, "Tried to access an invalid child.\n");
        exit(EXIT_FAILURE);
    }
    return child;
}

uint32_t *internal_node_key(void *node, uint32_t key_num) {
    return (uint32_t *)((char *)internal_node_cell(node, key_num) +
                        INTERNAL_NODE_CHILD_SIZE);
}

uint32_t get_node_max_key(Pager *pager, void *node) {
    NodeType type = get_node_type(node);
    if (type == NODE_LEAF) {
        uint32_t num_cells = *leaf_node_num_cells(node);
        if (num_cells == 0) {
            return 0;
        }
        return *leaf_node_key(node, num_cells - 1);
    }
    return get_node_max_key(pager,
                            get_page(pager, *internal_node_right_child(node)));
}

/** Return the address of the num_cells in the node page header */
uint32_t* leaf_node_num_cells(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_NUM_CELLS_OFFSET);
}

uint32_t *leaf_node_next_leaf(void *node) {
    return (uint32_t *)((char *)node + LEAF_NODE_NEXT_LEAF_OFFSET);
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
void initialize_leaf_node(void *node) {
    set_node_type(node, NODE_LEAF);
    set_node_root(node, false);
    *node_parent(node) = INVALID_PAGE_NUM;
    *leaf_node_num_cells(node) = 0;
    *leaf_node_next_leaf(node) = 0;
}

void initialize_internal_node(void *node) {
    set_node_type(node, NODE_INTERNAL);
    set_node_root(node, false);
    *node_parent(node) = INVALID_PAGE_NUM;
    *internal_node_num_keys(node) = 0;
    *internal_node_right_child(node) = INVALID_PAGE_NUM;
}

/* Insert a key/value pair into a leaf node*/
void leaf_node_insert(Cursor* cursor, uint32_t key, Row* value) {
    void *node = get_page(cursor->table->pager, cursor->page_num);

    uint32_t num_cells = *leaf_node_num_cells(node);
    if (num_cells >= LEAF_NODE_MAX_CELLS) {
        leaf_node_split_and_insert(cursor, key, value);
        return;
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

/* Split a full leaf and insert the new key into the appropriate half. */
void leaf_node_split_and_insert(Cursor *cursor, uint32_t key, Row *value) {
    Pager *pager = cursor->table->pager;
    if (pager->num_pages >= TABLE_MAX_PAGES) {
        /* execute_insert checks this before reaching here; keep this guard for
         * callers of the node API as well. */
        fprintf(stderr, "Error: Table full.\n");
        exit(EXIT_FAILURE);
    }

    void *old_node = get_page(pager, cursor->page_num);
    uint32_t old_max = get_node_max_key(pager, old_node);
    uint32_t new_page_num = get_unused_page_num(pager);
    void *new_node = get_page(pager, new_page_num);

    /* Sequential appends are common for integer primary keys.  Keep the
     * existing rightmost leaf full and place the appended row in a new leaf;
     * this preserves page capacity while arbitrary inserts still use the
     * balanced split below. */
    if (cursor->cell_num == LEAF_NODE_MAX_CELLS &&
        *leaf_node_next_leaf(old_node) == 0 && key > old_max) {
        initialize_leaf_node(new_node);
        *node_parent(new_node) = *node_parent(old_node);
        *leaf_node_next_leaf(old_node) = new_page_num;
        *leaf_node_num_cells(new_node) = 1;
        *leaf_node_key(new_node, 0) = key;
        serialize_row(value, leaf_node_value(new_node, 0));

        if (is_node_root(old_node)) {
            create_new_root(cursor->table, new_page_num);
        } else {
            internal_node_insert(cursor->table, *node_parent(old_node),
                                 new_page_num);
        }
        return;
    }

    initialize_leaf_node(new_node);
    *node_parent(new_node) = *node_parent(old_node);
    *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
    *leaf_node_next_leaf(old_node) = new_page_num;

    const uint32_t total_cells = LEAF_NODE_MAX_CELLS + 1;
    for (int32_t i = (int32_t)total_cells - 1; i >= 0; --i) {
        void *destination_node;
        uint32_t index_within_node;
        if ((uint32_t)i < LEAF_NODE_LEFT_SPLIT_COUNT) {
            destination_node = old_node;
            index_within_node = (uint32_t)i;
        } else {
            destination_node = new_node;
            index_within_node = (uint32_t)i - LEAF_NODE_LEFT_SPLIT_COUNT;
        }

        void *destination = leaf_node_cell(destination_node, index_within_node);
        if ((uint32_t)i == cursor->cell_num) {
            *leaf_node_key(destination_node, index_within_node) = key;
            serialize_row(value,
                          leaf_node_value(destination_node, index_within_node));
        } else {
            uint32_t source_index = (uint32_t)i > cursor->cell_num
                                        ? (uint32_t)i - 1
                                        : (uint32_t)i;
            memcpy(destination, leaf_node_cell(old_node, source_index),
                   LEAF_NODE_CELL_SIZE);
        }
    }

    *leaf_node_num_cells(old_node) = LEAF_NODE_LEFT_SPLIT_COUNT;
    *leaf_node_num_cells(new_node) = LEAF_NODE_RIGHT_SPLIT_COUNT;

    if (is_node_root(old_node)) {
        create_new_root(cursor->table, new_page_num);
        return;
    }

    uint32_t parent_page_num = *node_parent(old_node);
    void *parent = get_page(pager, parent_page_num);
    uint32_t old_child_index = internal_node_find_child(parent, old_max);
    if (old_child_index < *internal_node_num_keys(parent) &&
        *internal_node_child(parent, old_child_index) == cursor->page_num) {
        update_internal_node_key(parent, old_max,
                                 get_node_max_key(pager, old_node));
    }
    internal_node_insert(cursor->table, parent_page_num, new_page_num);
}

/* Return the child slot whose key range contains key. */
uint32_t internal_node_find_child(void *node, uint32_t key) {
    uint32_t min_index = 0;
    uint32_t max_index = *internal_node_num_keys(node);
    while (min_index < max_index) {
        uint32_t index = min_index + (max_index - min_index) / 2;
        if (*internal_node_key(node, index) >= key) {
            max_index = index;
        } else {
            min_index = index + 1;
        }
    }
    return min_index;
}

Cursor *internal_node_find(Table *table, uint32_t page_num, uint32_t key) {
    void *node = get_page(table->pager, page_num);
    uint32_t child_num = *internal_node_child(
        node, internal_node_find_child(node, key));
    void *child = get_page(table->pager, child_num);
    if (get_node_type(child) == NODE_LEAF) {
        return leaf_node_find(table, child_num, key);
    }
    return internal_node_find(table, child_num, key);
}

void update_internal_node_key(void *node, uint32_t old_key, uint32_t new_key) {
    uint32_t child_index = internal_node_find_child(node, old_key);
    uint32_t num_keys = *internal_node_num_keys(node);
    if (child_index >= num_keys) {
        fprintf(stderr, "Cannot update key for right child.\n");
        exit(EXIT_FAILURE);
    }
    *internal_node_key(node, child_index) = new_key;
}

void internal_node_insert(Table *table, uint32_t parent_page_num,
                          uint32_t child_page_num) {
    void *parent = get_page(table->pager, parent_page_num);
    void *child = get_page(table->pager, child_page_num);
    uint32_t child_max_key = get_node_max_key(table->pager, child);
    uint32_t original_num_keys = *internal_node_num_keys(parent);

    if (original_num_keys >= INTERNAL_NODE_MAX_KEYS) {
        internal_node_split_and_insert(table, parent_page_num, child_page_num);
        return;
    }

    uint32_t right_child_page_num = *internal_node_right_child(parent);
    if (right_child_page_num == INVALID_PAGE_NUM) {
        *internal_node_right_child(parent) = child_page_num;
        *node_parent(child) = parent_page_num;
        return;
    }

    uint32_t index = internal_node_find_child(parent, child_max_key);
    void *right_child = get_page(table->pager, right_child_page_num);
    *internal_node_num_keys(parent) = original_num_keys + 1;

    if (child_max_key > get_node_max_key(table->pager, right_child)) {
        *internal_node_child(parent, original_num_keys) = right_child_page_num;
        *internal_node_key(parent, original_num_keys) =
            get_node_max_key(table->pager, right_child);
        *internal_node_right_child(parent) = child_page_num;
    } else {
        for (uint32_t i = original_num_keys; i > index; --i) {
            memcpy(internal_node_cell(parent, i),
                   internal_node_cell(parent, i - 1),
                   INTERNAL_NODE_CELL_SIZE);
        }
        *internal_node_child(parent, index) = child_page_num;
        *internal_node_key(parent, index) = child_max_key;
    }
    *node_parent(child) = parent_page_num;
}

/* Split an internal node.  This path is not reached with the repository's
 * 100-page limit (one root can index all leaf pages), but keeps the B-tree
 * implementation complete for larger page limits. */
void internal_node_split_and_insert(Table *table, uint32_t parent_page_num,
                                    uint32_t child_page_num) {
    Pager *pager = table->pager;
    void *old_node = get_page(pager, parent_page_num);
    uint32_t old_max = get_node_max_key(pager, old_node);
    uint32_t new_page_num = get_unused_page_num(pager);
    bool splitting_root = is_node_root(old_node);
    uint32_t old_page_num = parent_page_num;
    void *parent;
    void *new_node = NULL;

    if (splitting_root) {
        /* create_new_root initializes the new right child and moves the old
         * root to a fresh left-child page. */
        create_new_root(table, new_page_num);
        parent = get_page(pager, table->root_num_page);
        old_page_num = *internal_node_child(parent, 0);
        old_node = get_page(pager, old_page_num);
    } else {
        parent = get_page(pager, *node_parent(old_node));
        new_node = get_page(pager, new_page_num);
        initialize_internal_node(new_node);
    }

    uint32_t *old_num_keys = internal_node_num_keys(old_node);
    uint32_t cur_page_num = *internal_node_right_child(old_node);
    void *cur = get_page(pager, cur_page_num);

    /* Move the old right child first, then move the largest keyed children. */
    internal_node_insert(table, new_page_num, cur_page_num);
    *node_parent(cur) = new_page_num;
    *internal_node_right_child(old_node) = INVALID_PAGE_NUM;

    for (int32_t i = (int32_t)INTERNAL_NODE_MAX_KEYS - 1;
         i > (int32_t)INTERNAL_NODE_MAX_KEYS / 2; --i) {
        cur_page_num = *internal_node_child(old_node, (uint32_t)i);
        cur = get_page(pager, cur_page_num);
        internal_node_insert(table, new_page_num, cur_page_num);
        *node_parent(cur) = new_page_num;
        --(*old_num_keys);
    }

    /* The child immediately left of the middle key becomes the old node's
     * right child; its separator key is promoted into the parent. */
    *internal_node_right_child(old_node) =
        *internal_node_child(old_node, *old_num_keys - 1);
    --(*old_num_keys);

    void *child = get_page(pager, child_page_num);
    uint32_t destination_page_num =
        get_node_max_key(pager, child) < get_node_max_key(pager, old_node)
            ? old_page_num
            : new_page_num;
    internal_node_insert(table, destination_page_num, child_page_num);
    *node_parent(child) = destination_page_num;

    uint32_t old_child_index = internal_node_find_child(parent, old_max);
    if (old_child_index < *internal_node_num_keys(parent) &&
        *internal_node_child(parent, old_child_index) == old_page_num) {
        update_internal_node_key(parent, old_max,
                                 get_node_max_key(pager, old_node));
    }
    if (!splitting_root) {
        internal_node_insert(table, *node_parent(old_node), new_page_num);
        *node_parent(new_node) = *node_parent(old_node);
    }
}

/* Return a Cursor according to the given page_num and key */
Cursor *leaf_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void *node = get_page(table->pager, page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    Cursor *cursor = malloc(CURSOR_SIZE);
    cursor->page_num = page_num;
    cursor->table = table;
    cursor->end_of_table = false;

    // Binary search
    uint32_t left = 0, right = num_cells;
    while(left < right) {
        uint32_t mid = left + (right - left) / 2;
        uint32_t mid_key = *leaf_node_key(node, mid);
        if(mid_key>key) {
            right = mid;
        }else if(mid_key<key) {
            left = mid + 1;
        } else {
            cursor->cell_num = mid;
            return cursor;
        }
    }
    cursor->cell_num = left;
    return cursor;
}

void create_new_root(Table *table, uint32_t right_child_page_num) {
    void *root = get_page(table->pager, table->root_num_page);
    void *right_child = get_page(table->pager, right_child_page_num);
    uint32_t left_child_page_num = get_unused_page_num(table->pager);
    void *left_child = get_page(table->pager, left_child_page_num);

    if (get_node_type(root) == NODE_INTERNAL) {
        initialize_internal_node(right_child);
    }

    /* The old root becomes the left child; root page 0 remains stable so
     * clients never need to update their table handle. */
    memcpy(left_child, root, PAGE_SIZE);
    set_node_root(left_child, false);
    *node_parent(left_child) = table->root_num_page;

    if (get_node_type(left_child) == NODE_INTERNAL) {
        uint32_t num_keys = *internal_node_num_keys(left_child);
        for (uint32_t i = 0; i <= num_keys; ++i) {
            uint32_t child_page_num = *internal_node_child(left_child, i);
            *node_parent(get_page(table->pager, child_page_num)) =
                left_child_page_num;
        }
    }

    initialize_internal_node(root);
    set_node_root(root, true);
    *internal_node_num_keys(root) = 1;
    *internal_node_child(root, 0) = left_child_page_num;
    *internal_node_key(root, 0) = get_node_max_key(table->pager, left_child);
    *internal_node_right_child(root) = right_child_page_num;
    *node_parent(right_child) = table->root_num_page;
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

static void print_indent(uint32_t indentation_level) {
    for (uint32_t i = 0; i < indentation_level; ++i) {
        printf("  ");
    }
}

void print_tree(Pager *pager, uint32_t page_num, uint32_t indentation_level) {
    void *node = get_page(pager, page_num);
    uint32_t num_keys;

    if (get_node_type(node) == NODE_LEAF) {
        num_keys = *leaf_node_num_cells(node);
        print_indent(indentation_level);
        printf("- leaf (size %u)\n", num_keys);
        for (uint32_t i = 0; i < num_keys; ++i) {
            print_indent(indentation_level + 1);
            printf("- %u\n", *leaf_node_key(node, i));
        }
        return;
    }

    num_keys = *internal_node_num_keys(node);
    print_indent(indentation_level);
    printf("- internal (size %u)\n", num_keys);
    for (uint32_t i = 0; i < num_keys; ++i) {
        print_tree(pager, *internal_node_child(node, i),
                   indentation_level + 1);
        print_indent(indentation_level + 1);
        printf("- key %u\n", *internal_node_key(node, i));
    }
    if (*internal_node_right_child(node) != INVALID_PAGE_NUM) {
        print_tree(pager, *internal_node_right_child(node),
                   indentation_level + 1);
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
        void *page = calloc(1, PAGE_SIZE);
        if (page == NULL) {
            fprintf(stderr, "Unable to allocate a database page.\n");
            exit(EXIT_FAILURE);
        }
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

uint32_t get_unused_page_num(Pager *pager) {
    if (pager->num_pages >= TABLE_MAX_PAGES) {
        fprintf(stderr, "No unused page remains in the table.\n");
        exit(EXIT_FAILURE);
    }
    return pager->num_pages;
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
        set_node_root(root_node, true);
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
    Cursor *cursor = table_find(table, 0);
    void *root_node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(root_node);
    cursor->end_of_table = (num_cells == 0);

    return cursor;
}

/* Return a Cursor at the given key or the last */
Cursor *table_find(Table*table, uint32_t key) {
    uint32_t root_page_num = table->root_num_page;
    void *root_node = get_page(table->pager, root_page_num);
    if(get_node_type(root_node)==NODE_LEAF) {
        return leaf_node_find(table, root_page_num, key);
    } else {
        return internal_node_find(table, root_page_num, key);
    }
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

    cursor->cell_num += 1;
    if (cursor->cell_num >= *leaf_node_num_cells(node)) {
        uint32_t next_page_num = *leaf_node_next_leaf(node);
        if (next_page_num == 0) {
            cursor->end_of_table = true;
        } else {
            cursor->page_num = next_page_num;
            cursor->cell_num = 0;
        }
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
        print_tree(table->pager, table->root_num_page, 0);
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
    if (strcmp(input_buffer->buffer, "insert") == 0 ||
        strncmp(input_buffer->buffer, "insert ", 7) == 0) {
        return prepare_insert(input_buffer, statement);
    } else if (strcmp(input_buffer->buffer, "select") == 0) {
        statement->type = STATEMENT_SELECT;
        return PREPARE_SUCCESS;
    } else {
        return PREPARE_UNRECOGNIZIED_STATEMENT;
    }
}

/** Executes an INSERT statement and stores the new row in its slot. */
EXECUTE_RESULT execute_insert(Statement* statement, Table* table) {
    Row *row_to_insert = &(statement->row_to_insert);
    uint32_t key_to_insert = row_to_insert->id;
    Cursor *cursor = table_find(table, key_to_insert);
    void *node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if(cursor->cell_num < num_cells) {
        uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num);
        if(key_at_index == key_to_insert) {
            free(cursor);
            return EXECUTE_DUPLICATE_KEY;
        }
    }

    if (num_cells >= LEAF_NODE_MAX_CELLS &&
        table->pager->num_pages >= TABLE_MAX_PAGES) {
        free(cursor);
        return EXECUTE_TABLE_FULL;
    }

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
