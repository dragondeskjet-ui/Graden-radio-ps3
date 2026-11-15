#ifndef JSON_H
#define JSON_H
/* Minimal JSON DOM parser (arena allocated). */
enum JType { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
struct JNode {
	JType type;
	double num;
	char *str;        /* J_STR; for object members: unused */
	char *key;        /* key if this node is an object member */
	JNode *child;     /* first child (arr/obj) */
	JNode *next;      /* next sibling */
};
struct JDoc { JNode *root; void *arena; };

JDoc *json_parse(const char *text, size_t len);   /* NULL on error */
void json_free(JDoc *d);
JNode *json_get(JNode *obj, const char *key);
#endif
