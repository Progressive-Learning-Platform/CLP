/*
    Copyright 2011 the PLP authors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

/* target specific handlers, one for each node op type */
/* each handle must call handle(node*) for children nodes, or just return */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parse_tree.h"
#include "symbol.h"
#include "log.h"
#include "code_gen.h"
#include "handlers.h"
#include "line.h"

/* the call/return instrcutions put 92 bytes of state on the stack, which sits between parameters and the local frame.
 * this gets added when looking up a parameter variable in get_offset and the o() macro
 */
#define PARAM_OFFSET 92
#define WORD 4

#define e(...) { v(n); sprintf(buffer, __VA_ARGS__); program = emit(program, buffer); }
#define v(x) {\
		 if (!is_visited(n->line) && ANNOTATE_SOURCE)\
		 {\
			 visit(n->line);\
			 sprintf(buffer, "#\n# LINE %d: %s#\n", n->line, get_line(n->line));\
			 program = emit(program, buffer);\
		 }\
	     }
#define o(x) (get_offset(x->t, x->id) + (adjust * WORD))
#define g(x) (is_global(x->t, x->id))
#define push(x) { e("push %s\n", x); adjust++; }
#define pop(x) { e("pop %s\n", x); adjust--; }
#define eq(x,y) (strcmp(x,y) == 0)

extern char *program;
char *strings = NULL;
extern symbol *labels;
extern symbol *constants;
extern int ANNOTATE_SOURCE;
char buffer[1024];
char Break_label[20];/*global to store label for break outside block*/
char Selection_label_done[20];/*global to store intermediate label for switch case*/
char Cont_label[20];/*global to store label for continue inside block*/
char Goto_label[20];/*global to store label for goto statement*/
char memassigned[20];
int scan_flag = 0;
int Print_flag = 0;
int array_depth = 0;
char arraynames[20][50];
int names = 0;
int arrayoffset[20];
int arraytype[20];
int strcpy_flag=0;
int adjust = 1;
int LVALUE = 0;
int params = 0;
int param_words = 0;
int locals = 0;
symbol *last_symbol = NULL;


void epilogue(node *n) {
	e("addiu $sp, $sp, %d\t# Epilogue\n", locals);
	e("move $sp, $s7\n");
	e("move $ra, $a3\n");
	e("return\n");
}

/* 0 = local, 1 = global */
int is_global(symbol_table *t, char *s) {
	symbol *curr;

	if (t == NULL) {
		err("[code_gen] undeclared variable: %s\n", s);
		return 0;
	}

	curr = t->s;
	
	while (curr != NULL) {
		if (strcmp(curr->value,s) == 0) {
			if (t->parent == NULL) {
				param_words = curr->size;
				return 1;
			} else {
				return 0;
			}
		}
		curr = curr->up;
	}
	return is_global(t->parent, s);
}

/* TODO: replace with get_symbol->offset */
int get_offset(symbol_table *t, char *s) {
	int offset = 0;
	symbol *curr;

	if (t == NULL) {
		err("code_gen] undeclared offset variable: %s \n", s);
		return offset;
	}

	curr = t->s;

	/* look up in the current symbol table, then up to the next, and so forth */
	while (curr != NULL) {
		if (strcmp(curr->value,s) == 0) {
			vlog("found symbol %s %s, size %d, attr %08x, ", curr->type, curr->value, curr->size, curr->attr);
			if (curr->attr & ATTR_PARAM)
				/* if this is a parameter, then add another 92 bytes to the offset, to get past the saved state of the caller */
				offset += PARAM_OFFSET;
			vlog("%d\n", offset);
			param_words = curr->size; /* this is needed in case a parameter is being passed */
			return offset;
		}
		if (!(curr->attr & ATTR_FUNCTION)) /* functions don't sit on the stack */
			offset += curr->size * WORD;
		curr = curr->up;
	}
	
	/* if we get here, we need to look up another table */
	return offset + get_offset(t->parent, s);
}

void handle_identifier(node *n) {
	if(strcmp(n->id,"NULL")==0){
		e("li $t0, 0\n");
		return;
	}
	last_symbol = find_symbol(n->t, n->id);
	if (LVALUE) {
		if (g(n)) {
			/* just grab the global named pointer */
			e("li $t0, %s # *%s\n", n->id, n->id);
		} else {
			if(strcmp(n->id,memassigned)==0){//hack for malloc
				/* grab the identifier and dereference it */
				e("lw $t0, %d($sp) # %s\n", o(n), n->id);
			}
			else{
				/* grab the identifier and put a pointer to it in t0 */
				e("addiu $t0, $sp, %d # *%s\n", o(n), n->id);
			}
		}
	} else {
		if (g(n)) {
			/* grab and dereference */
			e("li $t0, %s\n", n->id);
			e("lw $t0, 0($t0) # %s\n", n->id);
		} else {
			if(scan_flag ==1){
				e("addiu $t0, $t3, %d # *%s\n", o(n), n->id);
			}
			else if(strcpy_flag==1){
				e("addiu $t0, $sp, %d # *%s\n", o(n), n->id);
				strcpy_flag=0;
			}
			else{
				/* grab the identifier and dereference it */
				e("lw $t0, %d($sp) # %s\n", o(n), n->id);
			}	
		}
	}
}

void handle_constant(node *n) {
	e("li $t0, %s\n", n->id);
	//param_words = 1; /* constants are ALWAYS one word */
}

void handle_string(node *n) {
	char *s = gen_label();
	/* we use emit() to add to the string table */
	sprintf(buffer, "%s:\n", s);
	strings = emit(strings, buffer);
	sprintf(buffer, ".asciiw %s\n", n->id);
	strings = emit(strings, buffer);
	sprintf(buffer, ".space 1\n");
	strings = emit(strings, buffer);
	
	/* get a pointer to our string in $t0 */
	e("li $t0, %s\n", s);

	/* the translation unit handler will emit the strings string at the end of the program */
}

void handle_scanfint(node *n,char *s){
	char *UART_read_loop_label = gen_label();
	char *get_char_label = gen_label();
	char *UART_not_a_semicolon_label = gen_label();
	char *UART_not_a_space_label = gen_label();
	char *get_char_busy_wait_label = gen_label();

	e("li $s0, 0xF0000000	# UART\n");
	e("li $s2, 0b10	# Status register bit mask\n");
	e("li $t5, 10		# Used to convert decimal to binary\n");
	e("li $t6, 48		# Used to convert ASCII to decimal\n");
	e("li $t7, 59		# Used to store semi colon\n");
	e("li $t8, 32		# space character\n");
	e("li $t0, 0		# value of current number\n");
	
	e("%s:\n",UART_read_loop_label);
	e("	jal %s\n",get_char_label);
	e("	nop\n");
	e("# Check for end of input\n");
	e("	bne $v0, $t7, %s\n",UART_not_a_semicolon_label);
	e("		nop\n");
	
	e("		push $t0\n");
	e("		move $v0, $0\n");
	e("		j %s\n",s);
	e("		nop\n");
	e("%s:\n",UART_not_a_semicolon_label);
	e("# Check for space indicating end of number\n");
	e("	bne $v0, $t8, %s\n",UART_not_a_space_label);
	e("	nop\n");
	e("	#code for initializing variable\n");
	e("	push $t0\n");
	e("	move $t0, $0	#clear input\n");
	e("	j %s\n",UART_read_loop_label);
	e("	nop\n");
	e("# Assume value is a number, convert to binary\n");
	e("%s:\n",UART_not_a_space_label);
	e("	mullo $t0, $t0, $t5	#multiply current number by 10\n");
	e("	subu $v0, $v0, $t6	#convert UART character from ascii to decimal\n");
	e("	addu $t0, $t0, $v0\n");
	e("	j %s\n",UART_read_loop_label);
	
	e("#Description: Reads UART and returns it in $v0\n");
	e("# Uses s2, s4, v0 \n");
	e("%s:\n",get_char_label);
	e("	%s:\n",get_char_busy_wait_label);		
	e("		lw $s4, 4($s0)	#load status register\n");
	e("		and $s4, $s4, $s2	#mask for ready bit\n");
	e("		bne $s4, $s2, %s\n",get_char_label);
	e("		nop\n");
	e("	lw $v0, 8($s0)	#load from recieve buffer\n");
	e("	sw $s2, 0($s0)	#command register: clear status\n");
	e("	jr $ra\n");
	e("	nop\n");
}

void handle_scanfchar(node *n,char *s){
	char *UART_read_loop_label = gen_label();
	char *get_char_label = gen_label();
	char *UART_not_a_semicolon_label = gen_label();
	char *get_char_busy_wait_label = gen_label();

	e("li $s0, 0xF0000000	# UART\n");
	e("li $s2, 0b10	# Status register bit mask\n");
	e("li $t5, 10		# Used to convert decimal to binary\n");
	e("li $t6, 48		# Used to convert ASCII to decimal\n");
	e("li $t7, 59		# Used to store semi colon\n");
	e("li $t8, 32		# space character\n");
	e("li $t0, 0		# value of current number\n");
	
	e("%s:\n",UART_read_loop_label);
	e("	jal %s\n",get_char_label);
	e("	nop\n");
	e("# Check for end of input\n");
	e("	bne $v0, $t7, %s\n",UART_not_a_semicolon_label);
	e("	nop\n");
	
	e("	move $v0, $0\n");
	e("	j %s\n",s);
	e("	nop\n");
	e("%s:\n",UART_not_a_semicolon_label);
	e("	push $v0\n");

	e("	j %s\n",UART_read_loop_label);
	e("	nop\n");
	
	e("#Description: Reads UART and returns it in $v0\n");
	e("# Uses s2, s4, v0 \n");
	e("%s:\n",get_char_label);
	e("	%s:\n",get_char_busy_wait_label);		
	e("		lw $s4, 4($s0)	#load status register\n");
	e("		and $s4, $s4, $s2	#mask for ready bit\n");
	e("		bne $s4, $s2, %s\n",get_char_label);
	e("		nop\n");
	e("	lw $v0, 8($s0)	#load from recieve buffer\n");
	e("	sw $s2, 0($s0)	#command register: clear status\n");
	e("	jr $ra\n");
	e("	nop\n");
}

void handle_scanfstring(node *n,char *s, int num){
	char *UART_read_loop_label = gen_label();
	char *get_char_label = gen_label();
	char *UART_not_a_semicolon_label = gen_label();
	char *get_char_busy_wait_label = gen_label();
	char *assignmemorylocation_label = gen_label();
	char *incrementlocation_label = gen_label();
	char *pushop_label = gen_label();

	e("li $s0, 0xF0000000	# UART\n");
	e("li $s2, 0b10	# Status register bit mask\n");
	e("li $t5, 10		# Used to convert decimal to binary\n");
	e("li $t6, 48		# Used to convert ASCII to decimal\n");
	e("li $t7, 59		# Used to store semi colon\n");
	e("li $t8, 32		# space character\n");
	e("li $t0, 0		# value of current number\n");
	e("li $t4, %d         # number of arguments\n",num);
	e("li $a0, 0x100f0000	# Memory location for storing string\n");
	e("%s:\n",assignmemorylocation_label);
	e("li $s1, 0\n");
	e("lw $s3, 0($a0)\n");
	e("bne $s1, $s3, %s\n",incrementlocation_label);
	e("nop\n");
	e("li $s1, 0x100f0000\n");
	e("beq $a0, $s1, %s\n",pushop_label); 
	e("nop\n");
	e("addiu $a0, $a0, 4\n");
	e("j %s\n",pushop_label);
	e("nop\n");
	e("%s:\n",incrementlocation_label);
	e("addiu $a0, $a0, 4\n");
	e("j %s\n",assignmemorylocation_label);
	e("nop\n");
	e("%s:\n",pushop_label);
    	e("push $a0\n");
	
	e("%s:\n",UART_read_loop_label);
	e("	jal %s\n",get_char_label);
	e("	nop\n");
	e("# Check for end of input\n");
	e("	bne $v0, $t7, %s\n",UART_not_a_semicolon_label);
        e("	nop\n");
        e("	addiu $t4, $t4, -1\n");
        e("	addiu $a0, $a0, 4\n");
        e("	beq $t4, $0, %s\n",s);
        e("	nop\n");
        e("	push $a0\n");
	e("	j %s\n",UART_read_loop_label);
	e("	nop\n");
	e("%s:\n",UART_not_a_semicolon_label);
        e("	sw $v0, 0($a0)\n")
        e("	addiu $a0, $a0, 4\n");
	e("	j %s\n",UART_read_loop_label);
	e("	nop\n");
	
	e("#Description: Reads UART and returns it in $v0\n");
	e("# Uses s2, s4, v0 \n");
	e("%s:\n",get_char_label);
	e("	%s:\n",get_char_busy_wait_label);		
	e("		lw $s4, 4($s0)	#load status register\n");
	e("		and $s4, $s4, $s2	#mask for ready bit\n");
	e("		bne $s4, $s2, %s\n",get_char_label);
	e("		nop\n");
	e("	lw $v0, 8($s0)	#load from recieve buffer\n");
	e("	sw $s2, 0($s0)	#command register: clear status\n");
	e("	jr $ra\n");
	e("	nop\n");
}

void handle_postfix_expr(node *n) {
	int prev_lvalue;
	int i, offset;
	
	/* the first child is always grabbed as a reference */
	prev_lvalue = LVALUE;
	if(strcmp(n->children[0]->id, "plp_asm") == 0){
		printf("running for asm\n");
		e("jal %s\n",n->children[1]->children[0]->id);
		e("nop\n");
		return;
	}
	if(strcmp(n->children[0]->id, "strcat") == 0){
		char *check1_label = gen_label();
		char *check2_label = gen_label();
		char *done_label = gen_label();
		handle(n->children[1]->children[1]);
		e("move $t1, $t0\n");
		handle(n->children[1]->children[0]);
		e("%s:\n",check1_label);
		e("lw $t2, 0($t0)\n");
		e("beq $t2, $0, %s\n", check2_label);
		e("nop\n");
		e("addiu $t0, $t0, 4\n");
		e("j %s\n",check1_label);
		e("nop\n");
		e("%s:\n",check2_label);
		e("lw $t2, 0($t1)\n");
		e("beq $t2, $0, %s\n",done_label);
		e("nop\n");
		e("sw $t2, 0($t0)\n");
		e("addiu $t0, $t0, 4\n");
		e("addiu $t1, $t1, 4\n");
		e("j %s\n", check2_label);
		e("nop\n")
		e("%s:\n",done_label);
		e("sw $t2, 0($t0)\n");
		return;	
	}
	if(strcmp(n->children[0]->id, "strlen") == 0){/*handling strlen*/
		char *loop_label = gen_label();
		char *break_label = gen_label();
		handle(n->children[1]->children[0]);
		e("li $t1, 0\t#counter\n");
		e("%s:\n",loop_label);
		e("lw $t2, 0($t0)\n");
		e("beq $t2, $0, %s\n",break_label);
		e("nop\n");
		e("addiu $t0, $t0, 4\t#increment memory\n");
		e("addiu $t1, $t1, 1\t#increment counter\n");
		e("j %s\n",loop_label);
		e("nop\n");
		e("%s:\n",break_label);
		e("move $t0, $t1\n");
		return;
	}
	if(strcmp(n->children[0]->id, "strcpy") == 0){/*handling strcpy*/
		char *copy_label = gen_label();
		char *finish_label = gen_label();
		char space[30] = ".space 30";
		handle(n->children[1]->children[1]);
		e("move $t1, $t0\n");
		strcpy_flag=1;
		handle(n->children[1]->children[0]);
		strcpy_flag=0;
		e("move $t2, $t0\n");
		char *s = gen_label();
		/* we use emit() to add to the string table */
		sprintf(buffer, "%s:\n", s);
		sprintf(buffer + strlen(buffer), "%s\n", space);
		strings = emit(strings, buffer);
		/* get a pointer to our string in $t0 */
		e("li $t0, %s\n", s);
		e("sw $t0, 0($t2)\n");
		e("%s:\n",copy_label);
		e("lw $t2, 0($t1)\n");
		e("beq $t2, $0, %s\n",finish_label);
		e("nop\n");
		e("sw $t2, 0($t0)\n");
		e("addiu $t0, $t0, 4\n");
		e("addiu $t1, $t1, 4\n");
		e("j %s\n",copy_label);
		e("nop\n");
		e("%s:\n",finish_label);
		e("sw $t2, 0($t0)\n");
		return;	

	}
	if(strcmp(n->children[0]->id,"strcmp")==0){/*handling strcmp*/
		char *read_label = gen_label();
		char *finddiff_label = gen_label();
		handle(n->children[1]->children[1]);
		e("move $t1, $t0\n");
		handle(n->children[1]->children[0]);
		e("%s:\n",read_label);
		e("lw $t2, 0($t0)\t#read from string1\n");
		e("lw $t3, 0($t1)\t#read from string2\n");
		e("bne $t2, $t3, %s\n",finddiff_label);
		e("nop\n");
		e("beq $t2, $0, %s\n",finddiff_label);
		e("nop\n");
		e("beq $t3, $0, %s\n",finddiff_label);
		e("nop\n");
		e("addiu $t0, $t0, 4\n");
		e("addiu $t1, $t1, 4\n");
		e("j %s\n",read_label);
		e("nop\n");
		e("%s:\n",finddiff_label);
		e("subu $t0, $t2, $t3\n");
		return;
	}
	/*Code to handle scanf and printf*/
	if(strcmp(n->children[0]->id, "getchar") == 0){/*handling getchar*/
		if(strcmp(n->children[1]->id, "paren") != 0){
			lerr(n->line, "Wrong arguments to GETCHAR\n");
			return;
		}
		char *get_char_label = gen_label();
		char *get_char_busy_wait_label = gen_label();
		e("li $s0, 0xF0000000	# UART\n");
		e("li $s2, 0b10	# Status register bit mask\n");
		e("li $t0, 0		# value of current number\n");
	
		e("%s:\n",get_char_label);
		e("	%s:\n",get_char_busy_wait_label);		
		e("		lw $s4, 4($s0)	#load status register\n");
		e("		and $s4, $s4, $s2	#mask for ready bit\n");
		e("		bne $s4, $s2, %s\n",get_char_label);
		e("		nop\n");
		e("	lw $t0, 8($s0)	#load from recieve buffer\n");
		e("	sw $s2, 0($s0)	#command register: clear status\n");
		return;
	}
	if(strcmp(n->children[0]->id, "putchar") == 0){/*handling putchar*/
		char *libplp_uart_write_label = gen_label();
		char *libplp_uart_write_loop_label = gen_label();
		char *print_label = gen_label();
		int totalcharacters = n->children[1]->num_children;
		if(totalcharacters >1){
			lerr(n->line, "Wrong Number of arguments to PUTCHAR\n");
			return;
		}
		handle(n->children[1]->children[0]);
		e("move $a0, $t0\n");

		e("%s:\n",libplp_uart_write_label);		
		e("lui $t0, 0xf000		#uart base address\n");
		e("%s:\n",libplp_uart_write_loop_label);
		e("lw  $t1, 4($t0)		#get the uart status\n");
		e("andi $t1, $t1, 0x01	#mask for the cts bit\n");
		e("beq $t1, $zero, %s\n",libplp_uart_write_loop_label);
		e("nop\n");
		e("%s:\n",print_label);
		e("sw  $a0, 12($t0)	#write the data to the output buffer\n");
		e("sw  $t1, 0($t0)		#send the data!\n");
		e("move $t0, $a0\n");

		return;
	}
	if(strcmp(n->children[0]->id,"gets")==0){
		int total_items_scanned = n->children[1]->num_children;
		if(total_items_scanned>1){
			lerr(n->line, "Wrong Number of arguments to GETS\n");
			return;
		}
		char *UART_read_loop_label = gen_label();
		char *get_char_label = gen_label();
		char *UART_not_a_semicolon_label = gen_label();
		char *get_char_busy_wait_label = gen_label();
		char *assignmemorylocation_label = gen_label();
		char *incrementlocation_label = gen_label();
		char *pushop_label = gen_label();
		char *s = gen_label();
		scan_flag =1;
		e("move $t3, $sp\n");

		e("li $s0, 0xF0000000	# UART\n");
		e("li $s2, 0b10	# Status register bit mask\n");
		e("li $t7, 59		# Used to store semi colon\n");

		e("li $t0, 0		# value of current number\n");

		e("li $a0, 0x100f0000	# Memory location for storing string\n");
		e("%s:\n",assignmemorylocation_label);
		e("li $s1, 0\n");
		e("lw $s3, 0($a0)\n");
		e("bne $s1, $s3, %s\n",incrementlocation_label);
		e("nop\n");
		e("li $s1, 0x100f0000\n");
		e("beq $a0, $s1, %s\n",pushop_label); 
		e("nop\n");
		e("addiu $a0, $a0, 4\n");
		e("j %s\n",pushop_label);
		e("nop\n");
		e("%s:\n",incrementlocation_label);
		e("addiu $a0, $a0, 4\n");
		e("j %s\n",assignmemorylocation_label);
		e("nop\n");
		e("%s:\n",pushop_label);
	    	e("push $a0\n");
	
		e("%s:\n",UART_read_loop_label);
		e("	jal %s\n",get_char_label);
		e("	nop\n");
		e("# Check for end of input\n");
		e("	bne $v0, $t7, %s\n",UART_not_a_semicolon_label);
		e("	nop\n");
		e("	move $t1, $a0\n");
		e("	j %s\n",s);
		e("	nop\n");
		e("%s:\n",UART_not_a_semicolon_label);
		e("	sw $v0, 0($a0)\n")
		e("	addiu $a0, $a0, 4\n");
		e("	j %s\n",UART_read_loop_label);
		e("	nop\n");
	
		e("#Description: Reads UART and returns it in $v0\n");
		e("# Uses s2, s4, v0 \n");
		e("%s:\n",get_char_label);
		e("	%s:\n",get_char_busy_wait_label);		
		e("		lw $s4, 4($s0)	#load status register\n");
		e("		and $s4, $s4, $s2	#mask for ready bit\n");
		e("		bne $s4, $s2, %s\n",get_char_label);
		e("		nop\n");
		e("	lw $v0, 8($s0)	#load from recieve buffer\n");
		e("	sw $s2, 0($s0)	#command register: clear status\n");
		e("	jr $ra\n");
		e("	nop\n");
		e("%s:\n", s);
		handle(n->children[1]->children[0]);
		e("push $t0\n");
		e("move $t0, $t1\n");
		e("pop $t1\n");
		e("sw $t0, 0($t1)\n");
		scan_flag =0;
		return;
	}

	if(strcmp(n->children[0]->id, "puts") == 0){
		char *label = gen_label();
		char *libplp_uart_write_string_p3_label = gen_label();
		char *libplp_uart_write_p3_label = gen_label();
		char *libplp_uart_write_loop_p3_label = gen_label();
		char *libplp_uart_write_string_multi_word_p3_label = gen_label();
		char *libplp_uart_write_string_done_p3_label = gen_label();
		char *libplp_uart_write_string_loop_p3_label = gen_label();
		char *libplp_uart_newline_label = gen_label();
		int totalintegers = n->children[1]->num_children;
		if(totalintegers>1){
			lerr(n->line, "Wrong Number of arguments to PUTS\n");
			return;
		}
			handle(n->children[1]->children[0]);
			e("li $t6, 1\n");
			e("jal %s\n",libplp_uart_write_string_p3_label);
			e("nop\n");
						
		e("%s:\n",libplp_uart_newline_label);
	
		e("ori $a0, $zero, 0x000d	#newline\n");
		e("jal %s\n",libplp_uart_write_p3_label);
		e("nop\n");
		e("ori $a0, $zero, 0x000a  #linefeed\n");
		e("jal %s\n",libplp_uart_write_p3_label);
		e("nop\n");
		e("j %s\n",label);
		e("nop\n");
		e("%s:\n",libplp_uart_write_p3_label);
		e("lui $t0, 0xf000		#uart base address\n");
		e("%s:\n",libplp_uart_write_loop_p3_label);
		e("lw  $t1, 4($t0)		#get the uart status\n");
		e("andi $t1, $t1, 0x01	#mask for the cts bit\n");
		e("beq $t1, $zero, %s\n",libplp_uart_write_loop_p3_label);
		e("nop\n");
		e("sw  $a0, 12($t0)	#write the data to the output buffer\n");
		e("sw  $t1, 0($t0)		#send the data!\n");
		
		e("jr $31\n");
		e("nop\n");

		e("%s: 		#we have a pointer to the string in a0, just loop and increment until we see a null\n",libplp_uart_write_string_p3_label);
		e("move $t9, $31		#save the return address\n");
		e("move $t8, $t0		#save the argument\n");
		e("%s:\n",libplp_uart_write_string_multi_word_p3_label);
		e("lw $a0, 0($t8)		#first 1-4 characters\n");
		
		e("beq $a0, $zero, %s\n",libplp_uart_write_string_done_p3_label);
		e("nop\n");
		
		e("%s:\n",libplp_uart_write_string_loop_p3_label);
		e("jal %s	#write this byte\n",libplp_uart_write_p3_label);
		e("nop\n");
		

		e("j %s\n",libplp_uart_write_string_multi_word_p3_label);
		e("addiu $t8, $t8, 4	#increment for the next word\n");
		e("%s:\n",libplp_uart_write_string_done_p3_label);
		e("jr $t9			#go home\n");
		e("nop\n");
		e("%s:\n",label);
		e("move $t0, $t6\n");
		return;
	}
	if(strcmp(n->children[0]->id, "scanf") == 0){
		lerr(n->line, "scanf cannot be used\n\tUse plp_scanint or plp_scanchar\n\texample: plp_scanint(a) , plp_scanchar(a);\n");
	}
	if(strcmp(n->children[0]->id, "plp_scanchar")==0){
		int total_items_scanned = n->children[1]->num_children;
		char *s = gen_label();
		scan_flag =1;
		e("move $t3, $sp\n");
		handle_scanfchar(n->children[1],s);
		e("%s:\n", s);
		for(i= total_items_scanned-1;i>=0;i--){
			e("pop $t1\n");
			handle(n->children[1]->children[i]);
			e("push $t0\n");
			e("move $t0, $t1\n");
			e("pop $t1\n");
			e("sw $t0, 0($t1)\n");
			
		}
		scan_flag =0;
		return;
	}
	if(strcmp(n->children[0]->id, "plp_scanint")==0){
		int total_items_scanned = n->children[1]->num_children;
		char *s = gen_label();
		scan_flag =1;
		e("move $t3, $sp\n");
		handle_scanfint(n->children[1],s);
		e("%s:\n", s);
		for(i= total_items_scanned-1;i>=0;i--){
			e("pop $t1\n");
			handle(n->children[1]->children[i]);
			e("push $t0\n");
			e("move $t0, $t1\n");
			e("pop $t1\n");
			e("sw $t0, 0($t1)\n");
			
		}
		scan_flag =0;
		return;
	}
	if(strcmp(n->children[0]->id, "plp_scanstring")==0){
		int total_items_scanned = n->children[1]->num_children;
		char *s = gen_label();
		scan_flag =1;
		e("move $t3, $sp\n");
		handle_scanfstring(n->children[1],s, total_items_scanned);
		e("%s:\n", s);
		for(i= total_items_scanned-1;i>=0;i--){
			e("pop $t1\n");
			handle(n->children[1]->children[i]);
			e("push $t0\n");
			e("move $t0, $t1\n");
			e("pop $t1\n");
			e("sw $t0, 0($t1)\n");
			
		}
		scan_flag =0;
		return;
	}
	if(strcmp(n->children[0]->id, "printf") == 0){
		lerr(n->line, "printf cannot be used\n\tUse plp_printint\n\texample: plp_printint(a);\n");
	}
	if (strcmp(n->children[0]->id, "plp_printchar") == 0){
		char *label = gen_label();
		char *libplp_uart_write_label = gen_label();
		char *libplp_uart_write_loop_label = gen_label();
		char *print_label = gen_label();
		int totalcharacters = n->children[1]->num_children;
		for(i= 0;i<totalcharacters;i++){
			
			handle(n->children[1]->children[i]);
			e("move $a0, $t0\n");
			e("jal %s\n",libplp_uart_write_label);
			e("nop\n");
			
			
		}
		e("j %s\n",label);
		e("nop\n");
		e("%s:\n",libplp_uart_write_label);		
		e("lui $t0, 0xf000		#uart base address\n");
		e("%s:\n",libplp_uart_write_loop_label);
		e("lw  $t1, 4($t0)		#get the uart status\n");
		e("andi $t1, $t1, 0x01	#mask for the cts bit\n");
		e("beq $t1, $zero, %s\n",libplp_uart_write_loop_label);
		e("nop\n");
		e("%s:\n",print_label);
		e("sw  $a0, 12($t0)	#write the data to the output buffer\n");
		e("sw  $t1, 0($t0)		#send the data!\n");
		e("jr $ra\n");
		e("nop\n");
		e("%s:\n",label);
		return;
	}

	if(strcmp(n->children[0]->id, "plp_printint") == 0){
		char *label = gen_label();
		char *modulus_label = gen_label();
		char *align_label = gen_label();
		char *long_modulus_loop_label = gen_label();
		char *not_less_than_label = gen_label();
		char *not_equal_label = gen_label();
		char *modulus_exit_label = gen_label();
		char *SingleDigit_label = gen_label();
		char *libplp_uart_write_label = gen_label();
		char *libplp_uart_write_loop_label = gen_label();
		char *print_label = gen_label();
		char *negativenumber_label = gen_label();
		
		e("move $v1, $ra\n");
		e("li $0, 0\n");
		int totalintegers = n->children[1]->num_children;
		for(i= 0;i<totalintegers;i++){
			char *positive_label = gen_label();
			e("li $t6, 0\n");
			handle(n->children[1]->children[i]);
			e("slt $t3, $t0, $0\n");
			e("beq $t3, $0, %s\n",positive_label);
			e("nop\n"); 
			e("li $t6, 1\t#negative number flag\n");
			e("li $t1, -1\n");
			e("mullo $t0, $t0, $t1\n");
			e("%s:\n",positive_label);
			e("li $t1, 10\n");
			e("li $t5, 0\t#counter\n");
			e("jal %s\n",modulus_label);
			e("nop\n");			
		}
		e("j %s\n",label);
		e("nop\n");

		e("%s:\n",modulus_label);
		
		e("li $t4, 0 \n");
		e("li $v0, 0		# return value\n");
		e("li $t3, 0		# align shift count\n");

		e("%s:\n",align_label);
		e("beq $t1, $t0, %s\n",long_modulus_loop_label);	
		e("nop\n");	
		e("slt $t2, $t1, $t0	#t1 < t0\n");	
		e("beq $t2, $0, %s\n",long_modulus_loop_label);	
		e("nop\n");	
		e("sll $t1, $t1, 1\n");	
		e("addiu $t3, $t3, 1\n");	
		e("j %s\n",align_label);	
		e("nop\n");	

		e("%s:\n",long_modulus_loop_label);
		e("slt $t2, $t1, $t0	#t0 > t1\n");	
		e("beq $t2, $0, %s\n",not_less_than_label);	
		e("nop\n");		
		e("subu $t0, $t0, $t1\n");		
		e("%s:\n",not_less_than_label);	
		e("bne $t1, $t0, %s\n",not_equal_label);	
		e("nop\n");		
		e("addiu $t2, $t2, 1\n");		
		e("subu $t0, $t0, $t1\n");		
		e("%s:\n",not_equal_label);	
		e("nop\n");	
		e("sll $v0, $v0, 1	#shift result left\n");	
		e("addu $v0, $v0, $t2\n");	
		e("beq $t4, $t3, %s\n",modulus_exit_label);	
		e("nop\n");		
		
		e("srl $t1, $t1, 1\n");	
		e("addiu $t4, $t4, 1\n");	
		e("j %s\n",long_modulus_loop_label);	
		e("nop\n");	
		
		e("%s:\n",modulus_exit_label);
	
		e("addiu $t0, $t0, 48 #adding ascii value\n");
		e("push $t0\n");
		e("move $t0, $v0\n");
		e("addiu $t5, $t5 , 1\n");
		e("li $t1, 10\n");
		e("slt $t3, $t0, $t1\n");
		e("beq $t3, $zero, %s\n",modulus_label);
		e("nop\n");
		e("%s:\n",SingleDigit_label);
		e("addiu $t0, $t0, 48\n");
		e("push $t0\n");
		e("addiu $t5, $t5 , 1\n");
		e("beq $t6 ,$0, %s\n",libplp_uart_write_label);
		e("nop\n");
		e("li $t0, 45\n");
		e("push $t0\n");
		e("addiu $t5, $t5 , 1\n");
		e("%s:\n",libplp_uart_write_label);		
		e("lui $t0, 0xf000		#uart base address\n");
		e("%s:\n",libplp_uart_write_loop_label);
		e("lw  $t1, 4($t0)		#get the uart status\n");
		e("andi $t1, $t1, 0x01	#mask for the cts bit\n");
		e("beq $t1, $zero, %s\n",libplp_uart_write_loop_label);
		e("nop\n");
		e("%s:\n",print_label);
		e("pop $a0\n");
		e("sw  $a0, 12($t0)	#write the data to the output buffer\n");
		e("sw  $t1, 0($t0)		#send the data!\n");
		e("addiu $t5, $t5, -1		#decrement counter\n");
		e("bne $t5, $zero, %s\n",print_label);
		e("nop\n");
		e("jr $ra\n");
		e("nop\n");
		e("%s:\n",label);
		e("move $ra, $v1\n");
		return;	
	}
	if(strcmp(n->children[0]->id, "plp_printstring") == 0){
		char *label = gen_label();
		char *libplp_uart_write_string_p3_label = gen_label();
		char *libplp_uart_write_p3_label = gen_label();
		char *libplp_uart_write_loop_p3_label = gen_label();
		char *libplp_uart_write_string_multi_word_p3_label = gen_label();
		char *libplp_uart_write_string_done_p3_label = gen_label();
		char *libplp_uart_write_string_loop_p3_label = gen_label();
		char *libplp_uart_newline_label = gen_label();
		e("move $v1, $ra\n");
		e("li $0, 0\n"); 
		int totalintegers = n->children[1]->num_children;
		for(i= 0;i<totalintegers;i++){
			handle(n->children[1]->children[i]);
			e("jal %s\n",libplp_uart_write_string_p3_label);
			e("nop\n");
						
		}
		e("%s:\n",libplp_uart_newline_label);
	
		e("ori $a0, $zero, 0x000d	#newline\n");
		e("jal %s\n",libplp_uart_write_p3_label);
		e("nop\n");
		e("ori $a0, $zero, 0x000a  #linefeed\n");
		e("jal %s\n",libplp_uart_write_p3_label);
		e("nop\n");
		e("j %s\n",label);
		e("nop\n");
		e("%s:\n",libplp_uart_write_p3_label);
		e("lui $t0, 0xf000		#uart base address\n");
		e("%s:\n",libplp_uart_write_loop_p3_label);
		e("lw  $t1, 4($t0)		#get the uart status\n");
		e("andi $t1, $t1, 0x01	#mask for the cts bit\n");
		e("beq $t1, $zero, %s\n",libplp_uart_write_loop_p3_label);
		e("nop\n");
		e("sw  $a0, 12($t0)	#write the data to the output buffer\n");
		e("sw  $t1, 0($t0)		#send the data!\n");
		e("jr $31\n");
		e("nop\n");

		e("%s: 		#we have a pointer to the string in a0, just loop and increment until we see a null\n",libplp_uart_write_string_p3_label);
		e("move $t9, $31		#save the return address\n");
		e("move $t8, $t0		#save the argument\n");
		e("%s:\n",libplp_uart_write_string_multi_word_p3_label);
		e("lw $a0, 0($t8)		#first 1-4 characters\n");
		
		e("beq $a0, $zero, %s\n",libplp_uart_write_string_done_p3_label);
		e("nop\n");
		
		e("%s:\n",libplp_uart_write_string_loop_p3_label);
		e("jal %s	#write this byte\n",libplp_uart_write_p3_label);
		e("nop\n");
		

		e("j %s\n",libplp_uart_write_string_multi_word_p3_label);
		e("addiu $t8, $t8, 4	#increment for the next word\n");
		e("%s:\n",libplp_uart_write_string_done_p3_label);
		e("jr $t9			#go home\n");
		e("nop\n");
		e("%s:\n",label);
		e("move $ra, $v1\n");
	return;
	}
	if((strcmp(n->children[1]->id, "expression") == 0) &&(strcmp(n->children[0]->id, "postfix_expr") == 0)) {
		array_depth = 1;	
	}
	LVALUE = 1;
	handle(n->children[0]);
	LVALUE = prev_lvalue;

	if (strcmp(n->children[1]->id, "inc") == 0) {
		if (LVALUE) {
			lerr(n->line, "[code_gen] invalid lvalue\n");
		} else {
			/* post increment */
			e("lw $t1, 0($t0)\n");
			e("addiu $t2, $t1, 1\n");
			e("sw $t2, 0($t0)\n");
			e("move $t0, $t1\n");
		}
	} else if (strcmp(n->children[1]->id, "dec") == 0) {
		if (LVALUE) {
			lerr(n->line, "[code_gen] invalid lvalue\n");
		} else {
			/* post deccrement */
			e("lw $t1, 0($t0)\n");
			e("addiu $t2, $t1, -1\n");
			e("sw $t2, 0($t0)\n");
			e("move $t0, $t1\n");
		}
	} else if (strcmp(n->children[1]->id, "argument_expr_list") == 0) {
		/* function call */
		handle(n->children[1]);
		e("move $s6, $sp\n");
		e("call %s\n", n->children[0]->id);
		e("move $sp, $s6 \n");
		e("addiu $sp, $sp, %d\n", params * WORD);
		adjust = adjust-params;
		e("move $t0, $v0\n");
		params = 0;
	} else if (strcmp(n->children[1]->id, "paren") == 0) {
		/* function call without arguments */
		e("move $s6, $sp\n");
		e("call %s\n", n->children[0]->id);
		e("move $sp, $s6 \n");
		e("move $t0, $v0\n");
	} else if (strcmp(n->children[1]->id, "expression") == 0) {
		/* array index */
		if(array_depth==1){//for multidimentional array
			for(i=0;i<names;i++){
				if(strcmp(n->children[0]->id, arraynames[i])==0){
					offset = arrayoffset[i];
					if(arraytype[i] == 4){
						e("lw $t0, 0($t0)\n");
					}
					break;
				}

			}
			array_depth = 0;
			if (last_symbol != NULL) {
				if (last_symbol->attr & ATTR_POINTER) {
					e("lw $t0, 0($t0)\n");
				}
			}

			push("$t0");
			prev_lvalue = LVALUE;
			LVALUE = 0;
			handle(n->children[1]); /* the offset in the array */
			LVALUE = prev_lvalue;
			pop("$t1");
			e("li $t2, %d\n",offset);
			e("mullo $t0, $t0, $t2\n");
			e("sll $t0, $t0, 2\n"); /*TODO: not all array indexes are multiples of 4! */
			e("addu $t0, $t0, $t1\n");
			if (!LVALUE) { /* dereference it */
				e("lw $t0, 0($t0)\n");
			}
		}

			if (last_symbol != NULL) {
				if (last_symbol->attr & ATTR_POINTER) {
					e("lw $t0, 0($t0)\n");
				}
			}
			push("$t0");
			prev_lvalue = LVALUE;
			LVALUE = 0;
			if(scan_flag==1){//handling scanint for array members
				scan_flag=0;
				handle(n->children[1]); /* the offset in the array */
				pop("$t2");
				e("sll $t0, $t0, 2\n"); /*TODO: not all array indexes are multiples of 4! */
				e("addu $t0, $t0, $t2\n");
				
			}
			else{
				handle(n->children[1]); /* the offset in the array */
				LVALUE = prev_lvalue;
				pop("$t1");
				e("sll $t0, $t0, 2\n"); /*TODO: not all array indexes are multiples of 4! */
				e("addu $t0, $t0, $t1\n");
				if (!LVALUE) { /* dereference it */
					e("lw $t0, 0($t0)\n");
				}
			}
			
		//}
	} 
	else if (strcmp(n->children[1]->id, "dot") == 0)
	{
		/* puts address of struct member into $t0 */
		
		// offsets of struct and member within struct
		int member_offset = 0;
		
		// struct name
		char *struct_id = n->children[0]->id;
		// member name
		char *member_id = n->children[2]->id;
		
		// struct symbol table entry
		symbol *sym = find_symbol(n->children[0]->t, struct_id);
		// struct type
		char *struct_type = sym->type;
		// struct table entry
		struct_table *curr = find_struct(struct_type);
		if(curr == NULL)
		{
			lerr(n->line, "[code_gen] could not find struct type: %s\n", struct_type);
		}
		
		// Struct symbol table
		symbol_table *struct_sym_tbl = curr->s;
		// Struct member symbol pointer
		symbol *cur_sym;
		
		
		/*
		// DEBUG: Print struct identifier and member identifier
		printf("\t* Struct Name: %s, Member Name: %s\n", struct_id, member_id);
		
		// DEBUG: Print member name and type
		printf("\t\t* Attribute: %X, Type %s\n", sym->attr, sym->type);
		*/
		
		
		// if struct is a union (ATTR_UNION == 0x2000) then skip offset code
		if(sym->attr & ATTR_STRUCT)
		{
			// Determine member offset in struct
			cur_sym = struct_sym_tbl->s;
			
			while (strcmp(cur_sym->value, member_id) != 0) {
				if (cur_sym == NULL)
					lerr(n->line, "[code_gen] could not find member, %s, in struct: %s\n", member_id, struct_type);
				cur_sym = cur_sym->up;
				member_offset += 4;
			}
			e("addiu $t0, $t0, %d # offset of member: %s\n", member_offset, member_id);
		}
		else if(sym->attr & ATTR_UNION)
		{
			// No member offset is currently required for unions because all members are the same size and word aligned
		}
		else
		{
			lerr(n->line, "[code_gen] %s is not a struct or union so '.' cannot be used\n", struct_id);
		}
	}
	else if (strcmp(n->children[1]->id, "ptr") == 0)
	{
		/* puts value located in address of struct member into $t0 */
		
		
		// offsets of struct and member within struct
		int member_offset = 0;
		
		// struct name
		char *struct_id = n->children[0]->id;
		// member name
		char *member_id = n->children[2]->id;
		
		// struct symbol table entry
		symbol *sym = find_symbol(n->children[0]->t, struct_id);
		// struct type
		char *struct_type = sym->type;
		// struct table entry
		struct_table *curr = find_struct(struct_type);
		if(curr == NULL)
		{
			lerr(n->line, "[code_gen] could not find struct type: %s\n", struct_type);
		}
		
		// Struct symbol table
		symbol_table *struct_sym_tbl = curr->s;
		// Struct member symbol pointer
		symbol *cur_sym;
		
		
		// if struct is a union (ATTR_UNION == 0x2000) then skip offset code
		if(sym->attr & ATTR_STRUCT)
		{
			// Determine member offset in struct
			cur_sym = struct_sym_tbl->s;
			
			while (strcmp(cur_sym->value, member_id) != 0) {
				if (cur_sym == NULL)
					lerr(n->line, "[code_gen] could not find member, %s, in struct: %s\n", member_id, struct_type);
				cur_sym = cur_sym->up;
				member_offset += 4;
			}
			e("addiu $t0, $t0, %d # offset of member: %s\n", member_offset, member_id);
			if(strcmp(n->children[0]->id,"memassigned") != 1){	//hack for malloc
				e("lw $t0, 0($t0)\t# load value of %s->%s\n", struct_id, member_id);}
			//pop("$t1");
		}
		else if(sym->attr & ATTR_UNION)
		{
			// Mask only applicable bits of variable types that don't use full word (i.e. char, short, etc.)
			e("lw $t0, 0($t0)\t# load value of %s->%s\n", struct_id, member_id);
			cur_sym = find_symbol(struct_sym_tbl, member_id);
			if( strcmp(cur_sym->type, "char") == 0 )
			{
				e("andi $t0, $t0, 0xFF # Member is a char, Truncate value to 8 bits\n");
			}
			else if( strcmp(cur_sym->type, "short") == 0 )
			{
				e("andi $t0, $t0, 0xFF # Member is a short, Truncate value to 8 bits\n");
			}
			//pop("$t1");
		}
		else
		{
			lerr(n->line, "[code_gen] %s is not a struct or union so '->' cannot be used\n", struct_id);
		}
		
		
		// Old implementation without union
		/*
		// offsets of struct and member within struct
		int member_offset = 0;
		
		// struct name
		char *struct_id = n->children[0]->id;
		// struct type
		char *struct_type = find_symbol(n->children[0]->t, struct_id)->type;
		// member name
		char *member_id = n->children[2]->id;
		// struct symbol table
		struct_table *curr = find_struct(struct_type);
		
		if(curr == NULL)
		{
			lerr(n->line, "[code_gen] could not find struct type: %s\n", struct_type);
		}
		
		// Determine member offset in struct
		symbol *cur_sym = curr->s->s;
		
		while (strcmp(cur_sym->value, member_id) != 0) {
			if (cur_sym == NULL)
				lerr(n->line, "[code_gen] could not find member, %s, in struct type: %s\n", member_id, struct_type);
			cur_sym = cur_sym->up;
			member_offset += 4;
		}
		
		e("addiu $t0, $t0, %d\t# offset of member: %s\n", member_offset, member_id);
		e("lw $t0, 0($t0)\t# load value of %s->%s", struct_id, member_id);
		pop("$t1");
		*/
	}
	else
	{
		// Debug: display id of child that is not implemented
		// printf("id not implemented: %s\n", n->children[1]->id);
		lerr(n->line, "[code_gen] postfix expressions not fully implemented\n");
	}	
}

void handle_argument_expr_list(node *n) {
	int i;
	if(scan_flag==0 && Print_flag==0){
		for (i=n->num_children; i>0; i--) {
			handle(n->children[i-1]);
			push("$t0");
			vlog("[code_gen] pushed parameter with size %d words\n", param_words);
			params += param_words;
			param_words = 0;
		}
	}
	else{
	/*Code to handle Scanf and Printf*/
		if(scan_flag==1){
			/*Code to handle scanf*/
			printf("scanf\n");
			scan_flag=0;
		}
		else{
			/*Code to handle printf*/
			printf("printf\n");
			Print_flag = 0;
		}
	}
}

void handle_unary_expr(node *n) {
	if (strcmp(n->children[0]->id, "inc") == 0) {
		if (LVALUE) {
			lerr(n->line, "[code_gen] invalid lvalue\n");
		} else {
			/* preincrement the value and return it in t0 */
			LVALUE = 1;
			handle(n->children[1]);
			LVALUE = 0;
			e("lw $t1, 0($t0)\n");
			e("addiu $t1, $t1, 1\n");
			e("sw $t1, 0($t0)\n");
			e("move $t0, $t1\n");
		}
	} else if (strcmp(n->children[0]->id, "dec") == 0) {
		if (LVALUE) {
			lerr(n->line, "[code_gen] invalid lvalue\n");
		} else {
			/* predecrement the value and return it in t0 */
			LVALUE = 1;
			handle(n->children[1]);
			LVALUE = 0;
			e("lw $t1, 0($t0)\n");
			e("addiu $t1, $t1, -1\n");
			e("sw $t1, 0($t0)\n");
			e("move $t0, $t1\n");
		}
	} else if (strcmp(n->children[0]->id, "&") == 0) {
		int prev_lvalue = LVALUE;
		LVALUE = 1;
		handle(n->children[1]);
		LVALUE = prev_lvalue;
	} else if (strcmp(n->children[0]->id, "*") == 0) {
		int prev_lvalue = LVALUE;
		LVALUE = 0;
		handle(n->children[1]);
		if(n->children[1]->num_children==2 && strcmp(n->children[1]->children[1]->children[1]->id,"paren")==0){
			prev_lvalue=1;
		}
		LVALUE = prev_lvalue;
		if (!LVALUE) { /* dereference it */
			e("lw $t0, 0($t0)\n");
		}
	} else if (strcmp(n->children[0]->id, "+") == 0) {
		handle(n->children[1]);
		return;
		//lerr(n->line, "[code_gen] unary + is just silly\n");
	} else if (strcmp(n->children[0]->id, "-") == 0) {
		//lerr(n->line, "[code_gen] unary - is just silly\n");
		handle(n->children[1]);
		e("li $t1, -1\n");
		e("mullo $t0, $t0, $t1\n");
		return;
	} else if (strcmp(n->children[0]->id, "~") == 0) {
		/* bitwise invert */
		if (LVALUE) {
			lerr(n->line, "[code_gen] invalid lvalue\n");
		} else {
			handle(n->children[1]);
			e("nor $t0, $t0, $t0\n");
		}
	} else if (strcmp(n->children[0]->id, "!") == 0) {
		/* boolean true/false */
		if (LVALUE) {
			lerr(n->line, "[code_gen] invalid lvalue\n");
		} else {
			char *done = gen_label();
			handle(n->children[1]);
			e("move $t1, $t0\n");
			e("move $t0, $zero\n");
			e("bne $t1, $t0, %s\n", done);
			e("nop\n");
			e("ori $t0, $zero, 1\n");
			e("%s:\n", done);
		}
	} else {
		lerr(n->line, "[code_gen] unary expressions not fully implemented\n");
	}			
}

void handle_sizeof(node *n) {
	lerr(n->line, "[code_gen] handle_sizeof not implemented\n");
}

void handle_cast_expr(node *n) {
	if(n->children[1]->num_children>1){
		
		handle(n->children[1]);
		
	}

	return;
	lerr(n->line, "[code_gen] handle_cast_expr not implemented\n");
}

void handle_multiply(node *n) {
	handle(n->children[0]);
	push("$t0");
	handle(n->children[1]);
	pop("$t1");
	e("mullo $t0, $t0, $t1\n");	
}

void handle_divide(node *n) {
	char *division = gen_label();
	char *align = gen_label();
	char *long_division_loop = gen_label();
	char *not_less_than = gen_label();
	char *not_equal = gen_label();
	char *divison_exit = gen_label();
	handle(n->children[1]);
	e("move $t1, $t0\n");
	handle(n->children[0]);
	e("%s:\n",division);
	e("\n");
	e("li $v0, 0		# return value\n");
	e("li $t3, 0		# align shift count\n");
	e("li $t4, 0		# loop counter\n");

	e("%s:\n",align);
	e("beq $t1, $t0, %s\n",long_division_loop);	
	e("nop\n");	
	e("slt $t2, $t1, $t0	#t1 < t0\n");	
	e("beq $t2, $0, %s\n",long_division_loop);	
	e("nop\n");	
	e("sll $t1, $t1, 1\n");	
	e("addiu $t3, $t3, 1\n");	
	e("j %s\n",align);	
	e("nop\n");	

	e("%s:\n",long_division_loop);
	e("slt $t2, $t1, $t0	#t0 > t1\n");	
	e("beq $t2, $0, %s\n",not_less_than);	
	e("nop\n");		
	e("subu $t0, $t0, $t1\n");		
	e("%s:\n",not_less_than);	
	e("bne $t1, $t0, %s\n",not_equal);	
	e("nop\n");		
	e("addiu $t2, $t2, 1\n");		
	e("subu $t0, $t0, $t1\n");		
	e("%s:\n",not_equal);	
	e("nop\n");	
	e("sll $v0, $v0, 1	#shift result left\n");	
	e("addu $v0, $v0, $t2\n");	
	e("beq $t4, $t3, %s\n",divison_exit);	
	e("nop\n");	
		
	e("srl $t1, $t1, 1\n");	
	e("addiu $t4, $t4, 1\n");	
	e("j %s\n",long_division_loop);	
	e("nop\n");	
		
	e("%s:\n",divison_exit);
	e("move $t0, $v0\n");

}

void handle_mod(node *n) {
	char *modulus = gen_label();
	char *align = gen_label();
	char *long_modulus_loop = gen_label();
	char *not_less_than = gen_label();
	char *not_equal = gen_label();
	char *modulus_exit = gen_label();
	handle(n->children[1]);
	e("move $t1, $t0\n");
	handle(n->children[0]);

	e("%s:\n",modulus);
	e("\n");
	e("li $v0, 0		# return value\n");
	e("li $t3, 0		# align shift count\n");
	e("li $t4, 0		# loop counter\n");

	e("%s:\n",align);
	e("beq $t1, $t0, %s\n",long_modulus_loop);	
	e("nop\n");	
	e("slt $t2, $t1, $t0	#t1 < t0\n");	
	e("beq $t2, $0, %s\n",long_modulus_loop);	
	e("nop\n");	
	e("sll $t1, $t1, 1\n");	
	e("addiu $t3, $t3, 1\n");	
	e("j %s\n",align);	
	e("nop\n");	

	e("%s:\n",long_modulus_loop);
	e("slt $t2, $t1, $t0	#t0 > t1\n");	
	e("beq $t2, $0, %s\n",not_less_than);	
	e("nop\n");		
	e("subu $t0, $t0, $t1\n");		
	e("%s:\n",not_less_than);	
	e("bne $t1, $t0, %s\n",not_equal);	
	e("nop\n");		
	e("addiu $t2, $t2, 1\n");		
	e("subu $t0, $t0, $t1\n");		
	e("%s:\n",not_equal);	
	e("nop\n");	
	e("sll $v0, $v0, 1	#shift result left\n");	
	e("addu $v0, $v0, $t2\n");	
	e("beq $t4, $t3, %s\n",modulus_exit);	
	e("nop\n");	
		
	e("srl $t1, $t1, 1\n");	
	e("addiu $t4, $t4, 1\n");	
	e("j %s\n",long_modulus_loop);	
	e("nop\n");	
		
	e("%s:\n",modulus_exit);
}

void handle_add(node *n) {
	handle(n->children[0]);
	push("$t0");
	handle(n->children[1]);
	pop("$t1");
	e("addu $t0, $t0, $t1\n");
}

void handle_sub(node *n) {
	handle(n->children[0]);
	push("$t0");
	handle(n->children[1]);
	pop("$t1");
	e("subu $t0, $t1, $t0\n");
}

void handle_shift_left(node *n) {
	/* shifting is a mess with plp, as there is no r-type shift instruction.
	 * so we're *stuck* with a loop structure to shift... YECH! 
	 */
	char *loop = gen_label();
	char *done = gen_label();
	handle(n->children[1]); /* the shift amount */
	push("$t0");
	handle(n->children[0]); /* the thing to shift */
	pop("$t1");
	e("%s:\n", loop);
	e("beq $t1, $zero, %s\n", done);
	e("nop\n");
	e("sll $t0, $t0, 1\n");
	e("addiu $t1, $t1, -1\n");
	e("j %s\n", loop);
	e("nop\n");
	e("%s:\n", done);
}

void handle_shift_right(node *n) {
	/* shifting is a mess with plp, as there is no r-type shift instruction.
	 * so we're *stuck* with a loop structure to shift... YECH! 
	 */
	char *loop = gen_label();
	char *done = gen_label();
	handle(n->children[1]); /* the shift amount */
	push("$t0");
	handle(n->children[0]); /* the thing to shift */
	pop("$t1");
	e("%s:\n", loop);
	e("beq $t1, $zero, %s\n", done);
	e("nop\n");
	e("srl $t0, $t0, 1\n");
	e("addiu $t1, $t1, -1\n");
	e("j %s\n", loop);
	e("nop\n");
	e("%s:\n", done);
}

void handle_less_than(node *n) {
	/* TODO: HANDLE UNSIGNED COMPARE */
	/* return 1 if child 0 < child 1 */
	handle(n->children[0]);
	push("$t0");
	handle(n->children[1]);
	pop("$t1");
	e("slt $t0, $t1, $t0\n");
}

void handle_greater_than(node *n) {
	/* TODO: unsigned */
	/* return 1 if child 0 > child 1 */
	handle(n->children[0]);
	push("$t0");
	handle(n->children[1]);
	pop("$t1");
	e("slt $t0, $t0, $t1\n");
}

void handle_less_equal_than(node *n) {
	//lerr(n->line, "[code_gen] handle_less_equal_than not implemented\n");
    handle(n->children[0]);
    push("$t0");
    handle(n->children[1]);
    pop("$t1");
    e("slt $t0, $t0, $t1\n");           	// Determine if greater than
    e("nor $t0, $t0, $t0\t# $t0 = !$t0\n");	// Invert
    e("andi $t0, $t0, 1\t# Mask LSB\n")		// Mask LSB
}

void handle_greater_equal_than(node *n) {
	//lerr(n->line, "[code_gen] handle_greater_equal_than not implemented\n");
    handle(n->children[0]);
    push("$t0");
    handle(n->children[1]);
    pop("$t1");
    e("slt $t0, $t1, $t0\n");           	// Determine if less than
    e("nor $t0, $t0, $t0\t# $t0 = !$t0\n");	// Invert
    e("andi $t0, $t0, 1\t# Mask LSB\n")		// Mask LSB
}

void handle_equality(node *n) {
	/* TODO: implement a better equivalence routine */
	char *done = gen_label();
	handle(n->children[0]);
	push("$t0");
	handle(n->children[1]);
	pop("$t1");
	e("move $t2, $t0\n");
	e("move $t0, $zero\n");
	e("bne $t1, $t2, %s\n", done);
	e("nop\n");
	e("ori $t0, $zero, 1\n");
	e("%s:\n", done);
}

void handle_equality_not(node *n) {
	/* TODO: implement a better equivalence routine */
	char *done = gen_label();
	handle(n->children[0]);
	push("$t0");
	handle(n->children[1]);
	pop("$t1");
	e("move $t2, $t0\n");
	e("move $t0, $zero\n");
	e("beq $t1, $t2, %s\n", done);
	e("nop\n");
	e("ori $t0, $zero, 1\n");
	e("%s:\n", done);
}

void handle_bitwise_and(node *n) {
	handle(n->children[0]);
	push("$t0");
	handle(n->children[1]);
	pop("$t1");
	e("and $t0, $t0, $t1\n");
}

void handle_bitwise_xor(node *n) {
	lerr(n->line, "[code_gen] handle_bitwise_xor not implemented\n");
}

void handle_bitwise_or(node *n) {
	handle(n->children[0]);
	push("$t0");
	handle(n->children[1]);
	pop("$t1");
	e("or $t0, $t0, $t1\n");
}

void handle_logical_and(node *n) {
	char *done = gen_label();

	handle(n->children[0]);
	push("$t0");
	handle(n->children[1]);
	pop("$t1");

	e("move $t2, $t0\n");
	e("move $t0, $zero\n");
	e("beq $t1, $zero, %s\n", done);
	e("nop\n");
	e("beq $t2, $zero, %s\n", done);
	e("nop\n");
	e("ori $t0, $zero, 1\n");
	e("%s:\n", done);
}

void handle_logical_or(node *n) {
	char *done = gen_label();
	char *next = gen_label();

	handle(n->children[0]);
	push("$t0");
	handle(n->children[1]);
	pop("$t1");

	e("move $t2, $t0\n");
	e("move $t0, $zero\n");
	e("beq $t1, $zero, %s\n", next);
	e("nop\n");
	e("ori $t0, $zero, 1\n");
	e("%s:\n", next);
	e("beq $t2, $zero, %s\n", done);
	e("nop\n");
	e("ori $t0, $zero, 1\n");
	e("%s:\n", done);
}

void handle_conditional(node *n) {
	char *fail = gen_label();
	char *done = gen_label();
	handle(n->children[0]);
	e("beq $t0, $zero, %s\n", fail);
	e("nop\n");
	handle(n->children[1]);
	e("j %s\n", done);
	e("%s:\n", fail);
	handle(n->children[2]);
	e("%s:\n", done);
}

void handle_assignment(node *n) {
	/* get the lvalue */
	LVALUE = 1;
	handle(n->children[0]);
	LVALUE = 0;	

	push("$t0");

	/* and get the rvalue */
	handle(n->children[2]);

	pop("$t1");

	/* now make the assignment $t1 = $t0 */
	if (strcmp(n->children[1]->id, "assign") == 0) {
		e("sw $t0, 0($t1)\n");
	} else if (strcmp(n->children[1]->id, "assign_mul") == 0) {
		e("lw $t2, 0($t1)\n");
		e("mullo $t0, $t0, $t2\n");
		e("sw $t0, 0($t1)\n");
	} else if (strcmp(n->children[1]->id, "assign_div") == 0) {
		//lerr(n->line, "[code_gen] division not supported\n");
		char *division = gen_label();
		char *align = gen_label();
		char *long_division_loop = gen_label();
		char *not_less_than = gen_label();
		char *not_equal = gen_label();
		char *divison_exit = gen_label();
		e("lw $t2, 0($t1)\n");
		e("push $t1\n");
		e("move $t1, $t0 \n");
		e("move $t0, $t2 \n");
		
		e("%s:\n",division);e("\n");
		e("li $v0, 0		# return value\n");
		e("li $t3, 0		# align shift count\n");
		e("li $t4, 0		# loop counter\n");

		e("%s:\n",align);
		e("beq $t1, $t0, %s\n",long_division_loop);	
		e("nop\n");	
		e("slt $t2, $t1, $t0	#t1 < t0\n");	
		e("beq $t2, $0, %s\n",long_division_loop);	
		e("nop\n");	
		e("sll $t1, $t1, 1\n");	
		e("addiu $t3, $t3, 1\n");	
		e("j %s\n",align);	
		e("nop\n");	

		e("%s:\n",long_division_loop);
		e("slt $t2, $t1, $t0	#t0 > t1\n");	
		e("beq $t2, $0, %s\n",not_less_than);	
		e("nop\n");		
		e("subu $t0, $t0, $t1\n");		
		e("%s:\n",not_less_than);	
		e("bne $t1, $t0, %s\n",not_equal);	
		e("nop\n");		
		e("addiu $t2, $t2, 1\n");		
		e("subu $t0, $t0, $t1\n");		
		e("%s:\n",not_equal);	
		e("nop\n");	
		e("sll $v0, $v0, 1	#shift result left\n");	
		e("addu $v0, $v0, $t2\n");	
		e("beq $t4, $t3, %s\n",divison_exit);	
		e("nop\n");	
		
		e("srl $t1, $t1, 1\n");	
		e("addiu $t4, $t4, 1\n");	
		e("j %s\n",long_division_loop);	
		e("nop\n");	
		
		e("%s:\n",divison_exit);
		e("move $t0, $v0\n");
		e("pop $t1\n");
		e("sw $t0, 0($t1)\n");
	} else if (strcmp(n->children[1]->id, "assign_mod") == 0) {
		char *modulus = gen_label();
		char *align = gen_label();
		char *long_modulus_loop = gen_label();
		char *not_less_than = gen_label();
		char *not_equal = gen_label();
		char *modulus_exit = gen_label();
		e("lw $t2, 0($t1)\n");
		e("push $t1\n");
		e("move $t1, $t0 \n");
		e("move $t0, $t2 \n");

		e("%s:\n",modulus);
		e("\n");
		e("li $v0, 0		# return value\n");
		e("li $t3, 0		# align shift count\n");
		e("li $t4, 0		# loop counter\n");

		e("%s:\n",align);
		e("beq $t1, $t0, %s\n",long_modulus_loop);	
		e("nop\n");	
		e("slt $t2, $t1, $t0	#t1 < t0\n");	
		e("beq $t2, $0, %s\n",long_modulus_loop);	
		e("nop\n");	
		e("sll $t1, $t1, 1\n");	
		e("addiu $t3, $t3, 1\n");	
		e("j %s\n",align);	
		e("nop\n");	

		e("%s:\n",long_modulus_loop);
		e("slt $t2, $t1, $t0	#t0 > t1\n");	
		e("beq $t2, $0, %s\n",not_less_than);	
		e("nop\n");		
		e("subu $t0, $t0, $t1\n");		
		e("%s:\n",not_less_than);	
		e("bne $t1, $t0, %s\n",not_equal);	
		e("nop\n");		
		e("addiu $t2, $t2, 1\n");		
		e("subu $t0, $t0, $t1\n");		
		e("%s:\n",not_equal);	
		e("nop\n");	
		e("sll $v0, $v0, 1	#shift result left\n");	
		e("addu $v0, $v0, $t2\n");	
		e("beq $t4, $t3, %s\n",modulus_exit);	
		e("nop\n");	
		
		e("srl $t1, $t1, 1\n");	
		e("addiu $t4, $t4, 1\n");	
		e("j %s\n",long_modulus_loop);	
		e("nop\n");	
		
		e("%s:\n",modulus_exit);
		e("pop $t1\n");
		e("sw $t0, 0($t1)\n");
	} else if (strcmp(n->children[1]->id, "assign_add") == 0) {
		e("lw $t2, 0($t1)\n");
		e("addu $t0, $t0, $t2\n");
		e("sw $t0, 0($t1)\n");
	} else if (strcmp(n->children[1]->id, "assign_sub") == 0) {
		e("lw $t2, 0($t1)\n");
		e("subu $t0, $t2, $t0\n");
		e("sw $t0, 0($t1)\n");
	} else if (strcmp(n->children[1]->id, "assign_sll") == 0) {
		lerr(n->line, "[code_gen] shift assign not currently implemented\n");
	} else if (strcmp(n->children[1]->id, "assign_srl") == 0) {
		lerr(n->line, "[code_gen] shift assign not currently implemented\n");
	} else if (strcmp(n->children[1]->id, "assign_and") == 0) {
		e("lw $t2, 0($t1)\n");
		e("and $t0, $t0, $t2\n");
		e("sw $t0, 0($t1)\n");
	} else if (strcmp(n->children[1]->id, "assign_xor") == 0) {
		lerr(n->line, "[code_gen] xor assign not currently implemented\n");
	} else if (strcmp(n->children[1]->id, "assign_or") == 0) {
		e("lw $t2, 0($t1)\n");
		e("or $t0, $t0, $t2\n");
		e("sw $t0, 0($t1)\n");
	}
}

void handle_expression(node *n) {
	/* expressions can be one or more of:
		assignment
		conditional
		logical_or
		logical_and
		bitwise_or
		bitwise_xor
		bitwise_and
		equality
		equality_not
		less_than
		greater_than
		less_equal_than
		greater_equal_than
		shift_left
		shift_right
		add
		sub
		multiply
		divide
		mod
		cast_expr
		unary_expr
		sizeof
		postfix_expr
		(id,constant,string,(expression))
	*/
	/* there are handlers for all except id,constant,string */
	int i;
	for (i=0; i<n->num_children; i++)
		handle(n->children[i]);
}

void handle_declaration(node *n) {
	/* we only care to call the init_declarator_list */
	if (n->num_children == 2){
	/*need to change to allocate memory for static character array*/
		handle(n->children[0]);
		handle(n->children[1]);
	}
}

void handle_declaration_specifier(node *n) {
	char datatype[20];
	strcpy(datatype,n->children[0]->id);
	if(strcmp(datatype,"int")==0) return;
	else if (strcmp(datatype,"char")==0) return;
	else if(strcmp(datatype,"struct_union")==0) return;
	else if(strcmp(datatype,"void")==0) return;
	else lerr(n->line, "%s Datatype Not handled\n", datatype);
	//lerr(n->line, "[code_gen] handle_declaration_specifier not implemented\n");
}

void handle_init_declarator_list(node *n) {
	/* init_declarator_lists have one or more init_declarators */
	int i;
	for (i=0; i<n->num_children; i++)
		handle(n->children[i]);
}

void handle_init_declarator(node *n) {
	node *x;
	/* get the id node */
	if (n->children[0]->num_children == 1){
		x = n->children[0]->children[0]->children[0];
		if(n->children[0]->children[0]->num_children ==3){

			strcpy(arraynames[names],x->id);
			arrayoffset[names] = atoi(n->children[0]->children[0]->children[2]->id);
			names++;
		}
	}
	else
		x = n->children[0]->children[1]->children[0];

	if (g(x)) {
		/* global declaration */
		/* this is a special case, where we need to actually grab the constant value ourselves */
		/* By definition, global declarations may only be initialized to constants */
		e("%s:\n", x->id);
		if (n->num_children == 2) {
			/* this gets ugly...
			 * nodes are either initializer->constant or
			 * initializer->initializer_list->initializer->constant (with multiple initializers
			 */
			if (strcmp(n->children[1]->children[0]->id, "initializer_list") == 0) { /* mutliple initializers {1,2,3,4...} */
				int i;
				for (i=0; i<n->children[1]->children[0]->num_children; i++) {
					if (strcmp(n->children[1]->children[0]->children[i]->children[0]->id, "unary_expr") == 0) { /* this is a hack to detect negative numbers */
						e(".word -%s\n", n->children[1]->children[0]->children[i]->children[0]->children[1]->id);
					} else {
						e(".word %s\n", n->children[1]->children[0]->children[i]->children[0]->id);
					}
				}
			}
			else if(strcmp(n->children[1]->children[0]->id, "unary_expr")==0){//added for global unary minus
				
				e(".word 0x%X\n",-1*atoi(n->children[1]->children[0]->children[1]->id));	
			}
			else { /* single initializer */
				e(".word %s\n", n->children[1]->children[0]->id);
			}
		} else {
			/* it may still be an array declaration, just without an initializer, check the size on x */
			symbol *s = find_symbol(x->t, x->id);
			if (s == NULL) {
				e(".word 0\n");
			} else {
				e(".space %d\n", s->size);
			}
		}
	} else { /* a local variable initializer */
		/* init declarators may or may not have an initializer, if not, set the value to 0 */
		if (n->num_children == 2) { /* we have an initializer */
			 /* this gets ugly...
			 * nodes are either initializer->constant or
			 * initializer->initializer_list->initializer->constant (with multiple initializers
			 */
			if (strcmp(n->children[1]->children[0]->id, "initializer_list") == 0) { /* mutliple initializers {1,2,3,4...} */
				int i, prev;
				prev = o(x);
				for (i=0; i<n->children[1]->children[0]->num_children; i++) {
					if(strcmp(n->children[1]->children[0]->children[i]->children[0]->id, "initializer_list") == 0){//double dimentional array handling
						int j;
						for (j=0; j<n->children[1]->children[0]->children[i]->children[0]->num_children; j++) {	printf("6\n");
							
							if(n->children[1]->children[0]->children[i]->children[0]->children[j]->children[0]->type ==4){
								arraytype[--names] = 4;
								names++;

							} 
							handle(n->children[1]->children[0]->children[i]->children[0]->children[j]);
							
							e("sw $t0, %d($sp)\n", prev);
							prev +=  WORD;
						
						}
						
					}
					else{
						handle(n->children[1]->children[0]->children[i]);
						e("sw $t0, %d($sp)\n", o(x)+(WORD*i));
					}
				}
			} 
			else {
				handle(n->children[1]);
				e("sw $t0, %d($sp)\n", o(x));
				if(strcmp(n->children[1]->children[0]->id,"cast_expr")==0 || 
					(n->children[1]->children[0]->num_children==2 && strcmp(n->children[1]->children[0]->children[1]->id,"cast_expr")==0)){/*hack for malloc*/
						strcpy(memassigned,x->id);
				}
			}
		}
		else {
			/* we could initialize values to zero here, but we won't. */
			/*e("move $t0, $zero\n");
			e("sw $t0, %d($sp)\n", o(x));*/
		}
	}
}

void handle_struct_union(node *n) {
	lerr(n->line, "[code_gen] handle_struct_union not implemented\n");
}

void handle_struct_declaration_list(node *n) {
	lerr(n->line, "[code_gen] handle_struct_declaration_list not implemented\n");
}

void handle_struct_declaration(node *n) {
	lerr(n->line, "[code_gen] handle_struct_declaration not implemented\n");
}

void handle_specifier_qualifier_list(node *n) {
	lerr(n->line, "[code_gen] handle_specifier_qualifier_list not implemented\n");
}

void handle_struct_declarator_list(node *n) {
	lerr(n->line, "[code_gen] handle_struct_declarator_list not implemented\n");
}

void handle_struct_declarator(node *n) {
	lerr(n->line, "[code_gen] handle_struct_declarator not implemented\n");
}

void handle_enum_specifier(node *n) {
	lerr(n->line, "[code_gen] handle_enum_specifier not implemented\n");
}

void handle_enum_list(node *n) {
	lerr(n->line, "[code_gen] handle_enum_list not implemented\n");
}

void handle_enumerator(node *n) {
	lerr(n->line, "[code_gen] handle_enumerator not implemented\n");
}

void handle_declarator(node *n) {
	/* declarators can have a direct_declarator or a pointer / dd pair */
	if (n->num_children == 2)
		handle(n->children[1]);
	else
		handle(n->children[0]);
}

void handle_direct_declarator(node *n) {
	/* do nothing with direct declarators, the symbol already exists */
	/* this is the place to initialize direct declarators to 0 if we need to */
}

void handle_pointer(node *n) {
	lerr(n->line, "[code_gen] handle_pointer not implemented\n");
}

void handle_type_qualifier_list(node *n) {
	lerr(n->line, "[code_gen] handle_type_qualifier_list not implemented\n");
}

void handle_parameter_type_list(node *n) {
	lerr(n->line, "[code_gen] handle_parameter_type_list not implemented\n");
}

void handle_parameter_list(node *n) {
	lerr(n->line, "[code_gen] handle_parameter_list not implemented\n");
}

void handle_parameter_declaration(node *n) {
	lerr(n->line, "[code_gen] handle_parameter_declaration not implemented\n");
}

void handle_identifier_list(node *n) {
	lerr(n->line, "[code_gen] handle_identifier_list not implemented\n");
}

void handle_type_name(node *n) {
	return;
	handle(n->children[0]->children[0]);

	//lerr(n->line, "[code_gen] handle_type_name not implemented\n");
}

void handle_abstract_declarator(node *n) {
	lerr(n->line, "[code_gen] handle_abstract_declarator not implemented\n");
}

void handle_direct_abstract_declarator(node *n) {
	lerr(n->line, "[code_gen] handle_direct_abstract_declarator not implemented\n");
}

void handle_initializer(node *n) {
	handle(n->children[0]);
}

void handle_initializer_list(node *n) {
	lerr(n->line, "[code_gen] handle_initializer_list not implemented\n");
}

void handle_labeled_statement(node *n) {/*Only switch case implementation*/

	if (n->num_children == 2) { /* Case Statements*/
		if(n->children[0]->type==type_con) {
			char *selection_label_done = gen_label();
			strcpy(Selection_label_done,selection_label_done);
			push("$t0");
			handle(n->children[0]);
			pop("$t1");
			e("move $t2, $t0\n");
			e("move $t0, $t1\n");
			e("bne $t1, $t2, %s\n", selection_label_done);
			e("nop\n");
			handle(n->children[1]);
			
		}
		else{//label for jump statement
			e("%s:\n", n->children[0]->id);
			strcpy(Goto_label,n->children[0]->id);
			handle(n->children[1]);
		}
	} else if (n->num_children == 1){/*default statement*/
		char *done = gen_label();
		e("%s:\n", done);
	
		handle(n->children[0]);
	}else{
		lerr(n->line, "[code_gen] handle_labeled_statement not implemented\n");
	}
	
}

void handle_compound_statement(node *n) {
	/* compound statements have four types:
		no children
		statement_list
		declaration_list
		declaration_list statement_list
	*/
	
	/* but who cares, just call handle on the children */
	int i;
	for (i=0; i<n->num_children; i++)
		handle(n->children[i]);		
}

void handle_declaration_list(node *n) {
	/* just call on the children */
	int i;
	for (i=0; i<n->num_children; i++)
		handle(n->children[i]);
}

void handle_statement_list(node *n) {
	/* just a bunch of statements, call handle on them */
	int i;
	for (i=0; i<n->num_children; i++)
		handle(n->children[i]);
}

void handle_expression_statement(node *n) {
	/* it's either a ; or an expression */
	if (n->num_children == 1)
		handle(n->children[0]);
}

void handle_selection_statement(node *n) {
	if (n->num_children == 3) { /* if else statement */
		char *selection_label_else = gen_label();
		char *selection_label_done = gen_label();
		handle(n->children[0]);
		e("beq $t0, $zero, %s\n\tnop\n", selection_label_else);
		handle(n->children[1]);
		e("j %s\n\tnop\n", selection_label_done);
		e("%s:\n", selection_label_else);
		handle(n->children[2]);
		e("%s:\n", selection_label_done);
	} else if (n->num_children == 2){
		if(strcmp(n->children[1]->children[0]->children[0]->id,"labeled_statement")==0){/*switch case*/
			char *break_label = gen_label();
			strcpy(Break_label,break_label);
			handle(n->children[0]);
			handle(n->children[1]);
			e("%s:\n", Break_label);
		}
		else{/*only if with no else*/
			char *selection_label_done = gen_label();
			handle(n->children[0]);
			e("beq $t0, $zero, %s\n\tnop\n", selection_label_done);
			handle(n->children[1]);
			e("%s:\n", selection_label_done);
		}
	}else{
	//	lerr(n->line, "selection statement not fully implemented\n");
	}
}

void handle_iteration_statement(node *n) {
	/* iteration statements can be of four types:
	1	while   : expression statement
	2	do while: statement expression
	3	for     : expression_statement expression_statement statement
	4	for	: expression_statement expression_statement expression statement
	*/
	char *loop_label = gen_label();
	char *loop_label_done = gen_label();
	strcpy(Break_label,loop_label_done);
	if (strcmp(n->children[0]->id, "expression") == 0) {
		/* type 1, while loop */
		/* handle the expression first, then test, then execute the statement, then jump back */
		e("%s:\n", loop_label);
		strcpy(Cont_label,loop_label);
		handle(n->children[0]);
		e("beq $zero, $t0, %s\n", loop_label_done);
		e("nop\n");
		handle(n->children[1]);
		e("j %s\n", loop_label);
		e("nop\n");
		e("%s:\n", loop_label_done);
	} else if (n->num_children > 2) {
		char *cont_label = gen_label();
		strcpy(Cont_label,cont_label);
		/* type 3 or 4, for loop */ 
		handle(n->children[0]); /* initial expression */
		e("%s:\n", loop_label); /* beginning of for loop */
		handle(n->children[1]); /* condition, with result in $t0 */
		e("beq $t0, $zero, %s\n", loop_label_done);
		e("nop\n");
		if (n->num_children != 3) { /* statement */
			handle(n->children[3]); /* statement */
		}
		e("%s:\n", cont_label);
		handle(n->children[2]); /* last expression */
		e("j %s\n", loop_label);
		e("nop\n");
		e("%s:\n", loop_label_done);
	} else {
		e("%s:\n", loop_label);
		char *cont_label = gen_label();
		strcpy(Cont_label,cont_label);
		handle(n->children[0]);
		e("%s:\n", cont_label);
		handle(n->children[1]);
		e("beq $t0, $zero, %s\n", loop_label_done);
		e("nop\n");
		e("j %s\n", loop_label);
		e("nop\n");
		e("%s:\n", loop_label_done);
		//lerr(n->line, "this iteration statement not implemented yet");
	} 
}

void handle_jump_statement(node *n) {
	node *t = n->children[0];
	if (strcmp(t->id, "return") == 0) {
		if (n->num_children == 2)
			handle(n->children[1]);
			last_symbol = NULL; /* we want to support something like my_function()[5], which assumes that the pointer returned is just that, and doesn't need dereference magic */
			e("move $v0, $t0\n");
		//epilogue(n);
	}
	else if (strcmp(t->id, "break") == 0) {
		e("j %s\n", Break_label);
		e("nop\n");
		if(strlen(Selection_label_done)!=0){/*this code handles break in switch case*/
			e("%s:\n", Selection_label_done);
			strcpy(Selection_label_done,"\0");
		}	
	}else if (strcmp(t->id, "continue") == 0) {
		e("j %s\n", Cont_label);
		e("nop\n");
			
	}else if (strcmp(t->id, "goto") == 0) {
		e("j %s\n", Goto_label);
		e("nop\n");
	}
	 else {
		lerr(n->line, "[code_gen] jump statements not fully implemented\n");
	}
}

void handle_translation_unit(node *n) {
	/* translation units have external declarations and function declarations, just call handle on my children */
	int i;
	for (i=0; i<n->num_children; i++){	
		handle(n->children[i]);
	}

	/* emit the strings if there are any */
	if (strings != NULL) {
		e("%s\n", strings);
	}

}

void handle_function_definition(node *n) {
	/* function definitions are one of four types:
		specifiers declarator declarations compound
		specifiers declarator compound
		declarator declarations compound
		declarator compound
	*/
	int d_index;	
	char *function_name;
	int i;
	symbol_table *scope = NULL;
	symbol *curr;

	/* emit a label for the function name */
	if (strcmp(n->children[0]->id, "declarator") == 0)
		d_index = 0;
	else
		d_index = 1;
	
	if(n->children[d_index]->children[0]->num_children!=0){//added if to avoid segmentation fault
		function_name = n->children[d_index]->children[0]->children[0]->id;
	}
	else if(n->children[d_index]->children[1]->num_children!=0){//added to fix function name in case it returns pointer
		function_name = n->children[d_index]->children[1]->children[0]->id;
	}

	/* emit the function name */
	e("%s:\n",function_name);
	e("move $a3, $ra\n");//for saving return address
	/* reserve space on the stack for declarations related to this function (declarations only, parameters are pushed on the stack by the caller) */
	curr = n->t->s;
	while (strcmp(curr->value, function_name) != 0)
		curr = curr->up;
	/* start by finding the child symbol table that has assoc == n */
	for (i=0; i<n->t->num_children; i++) {
		if (n->t->children[i]->assoc == curr) {
			scope = n->t->children[i];
			break;
		}
	}
	/* add up the symbols */
	i=0;
	curr = scope->s;
	while (curr != NULL) {
		if (!(curr->attr & ATTR_PARAM))
			i += curr->size;
		curr = curr->up;
	}
	e("move $s7, $sp\n");
	e("addiu $sp, $sp, -%d\n", i*WORD);
	locals = i*WORD +4;
	
	/* call handle on the compound statement */
	handle(n->children[n->num_children-1]);

	epilogue(n);
}

void handle_asm(node *n) {
	/* the first child should just be a string, emit it */
	e("#inline assembly begin\n")
	e("%s\n",n->children[0]->id);
	e("#inline assembly end\n");
}
