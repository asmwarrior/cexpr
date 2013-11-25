#define _POSIX_C_SOURCE 200809L

#include "parse.h"
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define PARSE_PUSHBACK_BUF_SIZE 2

struct parse_state {
    lex_buf *buf;
    struct token push_back[PARSE_PUSHBACK_BUF_SIZE];
    char* error_message;
    struct parse_tree_node** nodelist;
    int nodes_allocated;
    int nodes_used;
};

static struct parse_tree_node* parse_comma(struct parse_state *state);


static struct token get_next_parse_token(struct parse_state* state) {

    for (int i = PARSE_PUSHBACK_BUF_SIZE - 1; i >= 0; --i) {
        if (state->push_back[i].token_type) {
            struct token ret = state->push_back[i];
            state->push_back[i].token_type = 0;
            return ret;
        }
    }

    return get_next_token(state->buf);

}

static void push_back(struct parse_state* state,
    struct token tok) {
    if (state->push_back[PARSE_PUSHBACK_BUF_SIZE-1].token_type) {
        printf("Programmer error: pushed back too many tokens\n");
        exit(1);
    }
    for (int i = 0; i < PARSE_PUSHBACK_BUF_SIZE; ++i) {
        if (state->push_back[i].token_type == 0) {
            state->push_back[i] = tok;
            break;
        }
    }
}

static struct parse_state make_parse_state(lex_buf* buf) {
    struct parse_state state = {.buf = buf};
    for (int i = 0; i < PARSE_PUSHBACK_BUF_SIZE; ++i) {
        state.push_back[i].token_type = 0;
    }
    state.error_message = 0;
    state.nodes_allocated = 100;
    state.nodelist = malloc(sizeof(struct parse_tree_node *) *
                            state.nodes_allocated);
    state.nodes_used = 0;
    return state;
}

static int match(struct token token, enum token_type expected_type) {
    return token.token_type == expected_type;
}

static void error(struct parse_state* state, const char* message, ...) {

    //compute size
    va_list args;
    va_start (args, message);
    int size = vsnprintf(0, 0, message, args);
    va_end (args);
    char* errbuf = malloc(size + 1);
    va_start (args, message);
    vsnprintf(errbuf, size + 1, message, args);
    va_end (args);

    state->error_message = errbuf;
}

static void free_tree_node(struct parse_tree_node *node) {
    if (node->text) {
        free((void*)node->text);
        node->text = 0;
    }
    if(node->left_child) {
        free_tree_node(node->left_child);
        node->left_child = 0;
    }
    if(node->right_child) {
        free_tree_node(node->right_child);
        node->right_child = 0;
    }
}

void free_result_tree(struct parse_result *result) {
    if (result->is_error) {
        free(result->error_message);
        return;
    }
    free_tree_node(result->node);
}

/* In the event of an error, we need to free the parse
   tree nodes that we have allocated */
static void free_parse_state(struct parse_state* state) {
    for (int i = 0; i < state->nodes_used; ++i) {
        free(state->nodelist[i]);
        state->nodelist[i] = 0;
    }
    state->nodes_used = 0;
}

static struct parse_tree_node* node_alloc(struct parse_state* state) {
    if (state->nodes_used == state->nodes_allocated) {
        state->nodes_allocated *= 2;
        state->nodelist = realloc(state->nodelist, state->nodes_allocated * sizeof(struct parse_state));
    }
    struct parse_tree_node* new_node = malloc(sizeof(struct parse_tree_node));
    state->nodelist[state->nodes_used++] = new_node;
    return new_node;
}

static struct parse_tree_node* make_node(struct parse_state* state,
                                         enum token_type op,
                                         struct parse_tree_node* left, 
                                         struct parse_tree_node* right) {

    struct parse_tree_node* node = node_alloc(state);

    node->op = op;
    node->left_child = left;
    node->right_child = right;
    return node;
}

static struct parse_tree_node* make_terminal_node(struct parse_state* state,
                                                  struct token token) {
    struct parse_tree_node* node = node_alloc(state);

    node->op = token.token_type;
    node->text = token.token_value;
    node->left_child = node->right_child = 0;
    return node;
}

static int is_assignop(struct token token) {
    switch (token.token_type) {
    case ASSIGN:
    case PERCENT_EQUAL:
    case CARET_EQUAL:
    case AMPERSAND_EQUAL:
    case BAR_EQUAL:
    case STAR_EQUAL:
    case MINUS_EQUAL:
    case PLUS_EQUAL:
    case SLASH_EQUAL:
    case LEFT_SHIFT_EQUAL:
    case RIGHT_SHIFT_EQUAL:
        return 1;
    default:
        return 0;
    }
}

static int is_unop(struct token token) {
    switch(token.token_type) {
    case AMPERSAND:
    case STAR:
    case PLUS:
    case MINUS:
    case BANG:
    case TILDE:
    case DOUBLE_PLUS:
    case DOUBLE_MINUS:
    case SIZEOF:
        return 1;
    default:
        return 0;
    }
}

static enum token_type binops[][5] = {
    {DOUBLE_BAR, 0},
    {DOUBLE_AMPERSAND, 0},
    {BAR, 0},
    {CARET, 0},

    {AMPERSAND, 0},
    {IS_EQUAL, BANG_EQUAL, 0},
    {LT, GT, LTE, GTE, 0},
    {LEFT_SHIFT, RIGHT_SHIFT, 0},

    {PLUS, MINUS, 0},
    {STAR, SLASH, PERCENT, 0},
    {0}
};

/* HACK: assume parenthesized expressions starting
 * with one of these are type casts
 */
static const char* type_name_starters[] = {
    "bool",
    "char",
    "double",
    "float",
    "int",
    "long",
    "off_t",
    "ptrdiff_t",
    "signed",
    "short",
    "size_t",
    "time_t",
    "unsigned",
    0
};

static int is_type_word(struct token token) {
    if (token.token_type != LITERAL_OR_ID) {
        return 0;
    }
    for (const char** type = type_name_starters; *type; ++type) {
        if (strcmp(*type, token.token_value) == 0) {
            return 1;
        }
    }
    return 0;
}

static char* append_token(char* existing, struct token token) {
    const char* token_str;
    if (token.token_value) {
        token_str = token.token_value;
    } else {
        token_str = token_names[token.token_type];
    }
    if (!existing) {
        return strdup(token_str);
    }
    int token_str_len = strlen(token_str);
    int existing_len = strlen(existing);
    char* new_str = realloc(existing, existing_len + token_str_len + 2);
    new_str[existing_len] = ' ';
    strcpy(new_str + existing_len + 1, token_str);
    return new_str;

}

static enum token_type closer(enum token_type opener) {
    if (opener == OPEN_PAREN) {
        return CLOSE_PAREN;
    } else if (opener == OPEN_BRACKET) {
        return CLOSE_BRACKET;
    } else {
        return -1;
    }
}

/* () [] . -> expr++ expr--	 */
static struct parse_tree_node* parse_primary_expression(struct parse_state *state) {
    struct token tok = get_next_parse_token(state);
    struct parse_tree_node* node = 0;
    switch(tok.token_type) {
    case OPEN_PAREN:
        node = parse_comma(state);
        if (!node) {
            return 0;
        }
        tok = get_next_parse_token(state);
        if (tok.token_type != CLOSE_PAREN) {
            error(state, "Missing ) parsing parenthesized expression");
            return 0;
        }
        break;
    case LITERAL_OR_ID:
        node = make_terminal_node(state, tok);
        break;
    default:
        error(state, "Unable to parse primary expression (token type is %s)",
              token_names[tok.token_type]);
        return 0;
    }

    bool more = true;
    while (more) {
        tok = get_next_parse_token(state);
        switch(tok.token_type) {
        case OPEN_PAREN:
        case OPEN_BRACKET:
        {
            struct parse_tree_node* right_node = parse_comma(state);
            if (!right_node) {
                return 0;
            }
            enum token_type close = closer(tok.token_type);
            tok = get_next_parse_token(state);
            if (tok.token_type != close) {
                error(state, "Missing %s", token_names[close]);
                return 0;
            }

            enum token_type op = close == CLOSE_PAREN ? FUNCTION_CALL : SUBSCRIPT;
            node = make_node(state, op, node, right_node);
            break;
        }
        case ARROW:
        case DOT:
        {
            enum token_type op = tok.token_type;
            tok = get_next_parse_token(state);
            if (tok.token_type != LITERAL_OR_ID) {
                error(state, "expected identifier before '%s' token",
                      token_names[tok.token_type]);
            }
            node = make_node(state, op, node, make_terminal_node(state, tok));
            break;
        }
        case DOUBLE_PLUS:
        case DOUBLE_MINUS:
        {
            node = make_node(state, tok.token_type, node, 0);
            break;
        }

        default:
            //not part of a primary expression
            more = false;
            push_back(state, tok);
            break;
        }
    }
    return node;
}

static char* parse_parenthesized_typename(struct token tok, struct parse_state* state) {
    //parse until close-paren
    char* typename = 0;
    while (tok.token_type != CLOSE_PAREN) {
        if (tok.token_type == END_OF_EXPRESSION) {
            return 0;
        }
        typename = append_token(typename, tok);
        tok = get_next_parse_token(state);
    }
    return typename;
}

/*
   * & + - ! ~ ++expr --expr (typecast) sizeof
 */
static struct parse_tree_node* parse_unop(struct parse_state *state) {

    struct parse_tree_node* node;

    struct token tok = get_next_parse_token(state);
    if (tok.token_type == SIZEOF) {
        tok = get_next_parse_token(state);
        if (tok.token_type == OPEN_PAREN) {
            //sizeof(typename)
            tok = get_next_parse_token(state);
            char* typename = parse_parenthesized_typename(tok, state);
            if (!typename) {
                error(state, "Found EOF when parsing (assumed) typecast");
                return 0;
            }
            node = make_node(state, SIZEOF, 0, 0);
            node->text = typename;
        } else {
            //sizeof var
            node = make_node(state, SIZEOF, 0, 0);
            node->text = strdup(tok.token_value);
        }

    } else if (is_unop(tok)) {
        struct parse_tree_node* right_node = parse_unop(state);
        if (!right_node) {
            return 0;
        }
        enum token_type token_type;
        switch (tok.token_type) {
        case STAR:
            token_type = DEREFERENCE;
            break;
        case AMPERSAND:
            token_type = REFERENCE;
            break;
        default:
            token_type = tok.token_type;
            break;
        }
        node = make_node(state, token_type, 0, right_node);
    } else if (tok.token_type == OPEN_PAREN) {
        //handle typecasts via hack
        struct token next_tok = get_next_parse_token(state);
        if (is_type_word(next_tok)) {
            char* typename = parse_parenthesized_typename(next_tok, state);
            if (!typename) {
                error(state, "Found EOF when parsing (assumed) typecast");
                return 0;
            }

            struct parse_tree_node* right_node = parse_unop(state);
            if (!right_node) {
                return 0;
            }
            node = make_node(state, TYPECAST, 0, right_node);
            node->text = typename;
        } else {
            push_back(state, next_tok);
            push_back(state, tok);
            //handle parenthesized expression
            node = parse_primary_expression(state);
            if (!node) {
                return 0;
            }
        }
    } else {
        push_back(state, tok);
        node = parse_primary_expression(state);
    }

    return node;
}

static int is_level_binop(struct token token, int level) {
    int i = 0;
    while (binops[level][i]) {
        if (token.token_type == binops[level][i++]) {
            return 1;
        }
    }
    return 0;
}

/*
 * Binary operations are all parsed the same way, but they're divided
 * into a bunch of different precedence levels.
 */
static struct parse_tree_node* parse_binop(struct parse_state *state, int level) {
    struct parse_tree_node* node;
    if (binops[level][0] == 0) {
        node = parse_unop(state);
        return node;
    } else {
        node = parse_binop(state, level + 1);
        if (!node) {
            return 0;
        }
    }
    while(1) {
        struct token tok = get_next_parse_token(state);
        if (tok.token_type == END_OF_EXPRESSION) {
            return node;
        }
        else if (tok.token_type == CLOSE_PAREN ||
                 tok.token_type == CLOSE_BRACKET) {
            push_back(state, tok);
            return node;
        }
        if (!is_level_binop(tok, level)) {
            push_back(state, tok);
            return node;
        }
        struct parse_tree_node *right_node = parse_binop(state, level + 1);
        if (!right_node) {
            return 0;
        }
        node = make_node(state, tok.token_type, node, right_node);
    }
}

static struct parse_tree_node* parse_ternop(struct parse_state *state) {
    struct parse_tree_node *left_node = parse_binop(state, 0);
    if (!left_node) {
        return 0;
    }

    struct token tok = get_next_parse_token(state);
    if (tok.token_type != QUESTION) {
        push_back(state, tok);
        return left_node;
    }

    struct parse_tree_node *mid_node = parse_ternop(state);
    if (!mid_node) {
        return 0;
    }

    tok = get_next_parse_token(state);
    if (tok.token_type != COLON) {
        error(state, "Missing : in ?: ternary op (found %s)",
              token_names[tok.token_type]);
        return 0;
    }
    struct parse_tree_node* right_node = parse_ternop(state);
    if (!right_node) {
        return 0;
    }

    struct parse_tree_node* new_right = make_node(state, COLON, mid_node, right_node);
    return make_node(state, QUESTION, left_node, new_right);
}

static struct parse_tree_node* parse_assignop(struct parse_state *state) {
    struct parse_tree_node *node = parse_ternop(state);
    if (!node) {
        return 0;
    }

    struct token tok = get_next_parse_token(state);
    if (!is_assignop(tok)) {
        push_back(state, tok);
        return node;
    }

    struct parse_tree_node *right_node = parse_assignop(state);
    if (!right_node) {
        return 0;
    }

    node = make_node(state, tok.token_type, node, right_node);
    return node;
}

static struct parse_tree_node* parse_comma(struct parse_state *state) {
    struct parse_tree_node *node = parse_assignop(state);
    if (!node)
        return 0;
    while (1) {
        struct token tok = get_next_parse_token(state);
        if (tok.token_type == END_OF_EXPRESSION ||
            tok.token_type == CLOSE_PAREN ||
            tok.token_type == CLOSE_BRACKET) {
            push_back(state, tok);
            return node;
        }
        if (!match(tok, COMMA)) {
            error(state, "Expected comma, got %s", token_names[tok.token_type]);
            return 0;
        }
        struct parse_tree_node *right_node = parse_assignop(state);
        if (!right_node) {
            return 0;
        }
        node = make_node(state, COMMA, node, right_node);
    }
}

struct parse_result* parse(const char* string) {
    lex_buf lex_buf = start_lex(string);
    struct parse_state state = make_parse_state(&lex_buf);
    struct parse_tree_node* node = parse_comma(&state);
    struct parse_result* result = malloc(sizeof(struct parse_result));
    if (!node) {
        result->is_error = true;
        result->error_message = state.error_message;
        free_parse_state(&state);
    } else {
        struct token tok = get_next_parse_token(&state);
        if (tok.token_type == END_OF_EXPRESSION) {
            result->is_error = false;
            result->node = node;
        } else {
            error(&state, "Unparsed portion of expression starts with %s",
                  token_names[tok.token_type]);
            result->is_error = true;
            result->error_message = state.error_message;
            free_parse_state(&state);
        }
    }

    return result;
}

/*
  Returns a pointer to the new end of the string.  Assumes
  that the buffer has enough space for the string.
 */
char* write_tree_to_string(struct parse_tree_node* node, char* buf) {
    switch (node->op) {
    case DEREFERENCE:
        assert (node->right_child);
        buf += sprintf(buf, "*(");
        buf = write_tree_to_string(node->right_child, buf);
        buf += sprintf(buf, ")");
        break;

    case REFERENCE:
        assert (node->right_child);
        buf += sprintf(buf, "&(");
        buf = write_tree_to_string(node->right_child, buf);
        buf += sprintf(buf, ")");
        break;

    case COMMA:
        assert (node->left_child);
        assert (node->right_child);

        buf = write_tree_to_string(node->left_child, buf);
        buf += sprintf(buf, ",");
        buf = write_tree_to_string(node->right_child, buf);
        break;

    case TYPECAST:
        buf += sprintf(buf, "((%s)", node->text);
        assert(!node->left_child);
        assert(node->right_child);
        buf = write_tree_to_string(node->right_child, buf);
        buf += sprintf(buf, ")");
        break;

    case FUNCTION_CALL:
        assert(node->left_child);
        assert(node->right_child);
        buf = write_tree_to_string(node->left_child, buf);
        buf += sprintf(buf, "(");
        buf = write_tree_to_string(node->right_child, buf);
        buf += sprintf(buf, ")");
        break;

    case SUBSCRIPT:
        assert(node->left_child);
        assert(node->right_child);
        buf = write_tree_to_string(node->left_child, buf);
        buf += sprintf(buf, "[");
        buf = write_tree_to_string(node->right_child, buf);
        buf += sprintf(buf, "]");
        break;

    case SIZEOF:
        buf += sprintf(buf, "sizeof(%s)", node->text);
        break;

    case LITERAL_OR_ID:
        buf += sprintf(buf, "%s", node->text);
        break;

    default:
        buf += sprintf(buf, "(");
        if (node->left_child) {
            buf = write_tree_to_string(node->left_child, buf);
        }
        buf += sprintf(buf, "%s", token_names[node->op]);
        if (node->right_child) {
            buf = write_tree_to_string(node->right_child, buf);
        }
        buf += sprintf(buf, ")");
        break;
    }
    return buf;
}
