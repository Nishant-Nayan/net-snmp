#include <net-snmp/net-snmp-config.h>

#if HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif

#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#if HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

static netsnmp_data_list *auto_tables;

typedef struct data_set_tables_s {
   netsnmp_table_data_set *table_set;
} data_set_tables;

typedef struct data_set_cache_s {
   void *data;
   size_t data_len;
} data_set_cache;

#define STATE_ACTION   1
#define STATE_COMMITED 2
#define STATE_UNDO     3
#define STATE_FREE     4
typedef struct newrow_stash_s {
   netsnmp_table_row *newrow;
   int state;
   int created;
} newrow_stash;

/** @defgroup table_dataset table_dataset: Helps you implement a table with datamatted storage.
 *  @ingroup handler
 *
 *  This handler helps you implement a table where all the data is
 *  expected to be stored within the agent itself and not in some
 *  external storage location.  It handles all MIB requests including
 *  GETs, GETNEXTs and SETs.  It's possible to simply create a table
 *  without actually ever defining a handler to be called when SNMP
 *  requests come in.  To use the data, you can either attach a
 *  sub-handler that merely uses/manipulates the data further when
 *  requests come in, or you can loop through it externally when it's
 *  actually needed.  This handler is most useful in cases where a
 *  table is holding configuration data for something which gets
 *  triggered via another event.
 *
 *  NOTE NOTE NOTE: This helper isn't complete and is likely to change
 *  somewhat over time.  Specifically, the way it stores data
 *  internally may change drastically.
 *  
 *  @{
 */

/** Create a netsnmp_table_data_set structure given a table_data definition */
netsnmp_table_data_set *
netsnmp_create_table_data_set(const char *table_name) 
{
    netsnmp_table_data_set *table_set = SNMP_MALLOC_TYPEDEF(netsnmp_table_data_set);
    table_set->table = netsnmp_create_table_data(table_name);
    return table_set;
}

/** Given a netsnmp_table_data_set definition, create a handler for it */
netsnmp_mib_handler *
get_netsnmp_table_data_set_handler(netsnmp_table_data_set *data_set)
{
    netsnmp_mib_handler *ret = NULL;

    if (!data_set) {
        snmp_log(LOG_INFO, "get_netsnmp_table_data_set_handler(NULL) called\n");
        return NULL;
    }
    
    ret = netsnmp_create_handler(TABLE_DATA_SET_NAME, netsnmp_table_data_set_helper_handler);
    if (ret) {
        ret->myvoid = (void *) data_set;
    }
    return ret;
}


/** register a given data_set at a given oid (specified in the
    netsnmp_handler_registration pointer).  The
    reginfo->handler->access_method *may* be null if the call doesn't
    ever want to be called for SNMP operations.
*/
int
netsnmp_register_netsnmp_table_data_set(netsnmp_handler_registration *reginfo, netsnmp_table_data_set *data_set,
                        netsnmp_table_registration_info *table_info)
{
    if (NULL == table_info) {
        /* allocate the table if one wasn't allocated */
        table_info = SNMP_MALLOC_TYPEDEF(netsnmp_table_registration_info);
    }

    if (NULL == table_info->indexes && data_set->table->indexes_template) {
        /* copy the indexes in */
        table_info->indexes =
            snmp_clone_varbind(data_set->table->indexes_template);
    }
    
    if ((!table_info->min_column || !table_info->max_column) &&
        (data_set->default_row)) {
        /* determine min/max columns */
        unsigned int mincol = 0xffffffff, maxcol = 0;
        netsnmp_table_data_set_storage *row;
        
        for(row = data_set->default_row; row; row = row->next) {
            mincol = SNMP_MIN(mincol, row->column);
            maxcol = SNMP_MAX(maxcol, row->column);
        }
        if (!table_info->min_column)
            table_info->min_column = mincol;
        if (!table_info->max_column)
            table_info->max_column = maxcol;
    }

    netsnmp_inject_handler(reginfo, get_netsnmp_table_data_set_handler(data_set));
    return netsnmp_register_table_data(reginfo, data_set->table, table_info);
}

/** Finds a column within a given storage set, given the pointer to
   the start of the storage set list.
*/
netsnmp_table_data_set_storage *
netsnmp_table_data_set_find_column(netsnmp_table_data_set_storage *start, unsigned int column) 
{
    while(start && start->column != column)
        start = start->next;
    return start;
}

/**
 * extracts a netsnmp_table_data_set pointer from a given request
 */
inline netsnmp_table_data_set *
extract_netsnmp_table_data_set(netsnmp_request_info *request)
{
    return (netsnmp_table_data_set *)
        netsnmp_request_get_list_data(request, TABLE_DATA_SET_NAME);
}

/**
 * marks a given column in a row as writable or not.
 */
int
netsnmp_mark_row_column_writable(netsnmp_table_row *row, int column, int writable) 
{
    netsnmp_table_data_set_storage *data = (netsnmp_table_data_set_storage *) row->data;
    data = netsnmp_table_data_set_find_column(data, column);

    if (!data) {
        /* create it */
        data = SNMP_MALLOC_TYPEDEF(netsnmp_table_data_set_storage);
        if (!data) {
            snmp_log(LOG_CRIT, "no memory in netsnmp_set_row_column");
            return SNMPERR_MALLOC;
        }
        data->column = column;
        data->writable = writable;
        data->next = row->data;
        row->data = data;
    } else {
        data->writable = writable;
    }
    return SNMPERR_SUCCESS;
}


/**
 * sets a given column in a row with data given a type, value, and
 * length.  Data is memdup'ed by the function.
 */
int
netsnmp_set_row_column(netsnmp_table_row *row, unsigned int column, int type,
               const char *value, size_t value_len) 
{
    netsnmp_table_data_set_storage *data = (netsnmp_table_data_set_storage *) row->data;
    data = netsnmp_table_data_set_find_column(data, column);

    if (!data) {
        /* create it */
        data = SNMP_MALLOC_TYPEDEF(netsnmp_table_data_set_storage);
        if (!data) {
            snmp_log(LOG_CRIT, "no memory in netsnmp_set_row_column");
            return SNMPERR_MALLOC;
        }
        
        data->column = column;
        data->type = type;
        data->next = row->data;
        row->data = data;
    }
    
    if (value) {
        if (data->type != type)
            return SNMPERR_GENERR;
        
        SNMP_FREE(data->data.voidp);
        if (value_len) {
            if (memdup(&data->data.string, value, (value_len)) !=
                SNMPERR_SUCCESS) {
                snmp_log(LOG_CRIT, "no memory in netsnmp_set_row_column");
                return SNMPERR_MALLOC;
            }
        } else {
            data->data.string = malloc(1);
        }
        data->data_len = value_len;
    }
    return SNMPERR_SUCCESS;
}

/** adds a new default row to a table_set.
 * Arguments should be the table_set, column number, variable type and
 * finally a 1 if it is allowed to be writable, or a 0 if not.  If the
 * default_value field is not NULL, it will be used to populate new
 * valuse in that column fro newly created rows. It is copied into the
 * storage template (free your calling argument).
 *
 * returns SNMPERR_SUCCESS or SNMPERR_FAILURE
 */
int
netsnmp_table_set_add_default_row(netsnmp_table_data_set *table_set,
                                  unsigned int column,
                                  int type, int writable,
                                  void *default_value,
                                  size_t default_value_len) 
{
    
    netsnmp_table_data_set_storage *new_col, *ptr;

    /* double check */
    new_col = netsnmp_table_data_set_find_column(table_set->default_row, column);
    if (new_col != NULL) {
        if (new_col->type == type &&
            new_col->writable == writable)
            return SNMPERR_SUCCESS;
        return SNMPERR_GENERR;
    }

    new_col = SNMP_MALLOC_TYPEDEF(netsnmp_table_data_set_storage);
    new_col->type = type;
    new_col->writable = writable;
    new_col->column = column;
    if (default_value) {
        memdup((u_char **) &(new_col->data.voidp),
               (u_char *) default_value, default_value_len);
        new_col->data_len = default_value_len;
    }
    if (table_set->default_row == NULL)
        table_set->default_row = new_col;
    else {
        for(ptr = table_set->default_row; ptr->next; ptr = ptr->next) {
        }
        ptr->next = new_col;
    }
    return SNMPERR_SUCCESS;
}

/** clones a dataset row, including all data. */
netsnmp_table_row *
netsnmp_table_data_set_clone_row(netsnmp_table_row *row) 
{
    netsnmp_table_data_set_storage *data, **newrowdata;
    netsnmp_table_row *newrow = 
        netsnmp_table_data_clone_row(row);

    if (!newrow)
        return NULL;
       
    data = (netsnmp_table_data_set_storage *) row->data;
    
    if (data) {
        for(newrowdata = (netsnmp_table_data_set_storage **) &newrow->data;
            data;
            newrowdata = &((*newrowdata)->next), data = data->next) {
            
            memdup((u_char **) newrowdata, (u_char *) row->data,
                   sizeof(netsnmp_table_data_set_storage));
            if (!*newrowdata)
                return NULL;
            memdup((u_char **) (*newrowdata)->data.voidp,
                   (u_char *) data->data.voidp,
                   data->data_len);
            if (!(*newrowdata)->data.voidp)
                return NULL;
        }
    }
    return newrow;
}

/** creates a new row from an existing defined default set */
netsnmp_table_row *
netsnmp_table_data_set_create_row_from_defaults(netsnmp_table_data_set_storage *defrow)
{
    netsnmp_table_row *row;
    row = netsnmp_create_table_data_row();
    if (!row)
        return NULL;
    for(; defrow; defrow = defrow->next) {
        netsnmp_set_row_column(row, defrow->column, defrow->type,
                               defrow->data.voidp, defrow->data_len);
        if (defrow->writable)
            netsnmp_mark_row_column_writable(row, defrow->column, 1);
        
    }
    return row;
}

/** implements the table data helper.  This is the routine that takes
 *  care of all SNMP requests coming into the table. */
int
netsnmp_table_data_set_helper_handler(
    netsnmp_mib_handler               *handler,
    netsnmp_handler_registration      *reginfo,
    netsnmp_agent_request_info        *reqinfo,
    netsnmp_request_info              *requests) {

    netsnmp_table_data_set_storage *data = NULL;
    newrow_stash *newrowstash;
    netsnmp_table_row *row, *newrow;
    netsnmp_table_request_info *table_info;
    netsnmp_request_info *request;
    oid *suffix;
    size_t suffix_len;
    netsnmp_oid_stash_node **stashp;

    DEBUGMSGTL(("netsnmp_table_data_set", "handler starting\n"));
    for(request = requests; request; request = request->next) {
        netsnmp_table_data_set *datatable =
            (netsnmp_table_data_set *) handler->myvoid;
        if (request->processed)
            continue;

        /* extract our stored data and table info */
        row = netsnmp_extract_netsnmp_table_row(request);
        table_info = netsnmp_extract_table_info(request);
        suffix = requests->requestvb->name + reginfo->rootoid_len + 2;
        suffix_len = requests->requestvb->name_length -
            (reginfo->rootoid_len + 2);

        if (MODE_IS_SET(reqinfo->mode)) {
            /* use a cached copy of the row for modification */

            /* cache location: may have been created already by other
               SET requests in the same master request. */
            stashp = (netsnmp_oid_stash_node **)
                netsnmp_table_get_or_create_row_stash(reqinfo,
                                                      "dataset_row_stash");

            newrowstash
                = netsnmp_oid_stash_get_data(*stashp, suffix, suffix_len);

            if (!newrowstash) {
                if (!row) {
                    /* entirely new row.  Create the row from the template */
                    newrowstash = SNMP_MALLOC_TYPEDEF(newrow_stash);
                    newrowstash->created = 1;
                    newrow = netsnmp_table_data_set_create_row_from_defaults(datatable->default_row);
                    newrow->indexes = snmp_clone_varbind(table_info->indexes);
                    newrowstash->newrow = newrow;
                } else {
                    /* existing row that needs to be modified */
                    newrowstash = SNMP_MALLOC_TYPEDEF(newrow_stash);
                    newrow = netsnmp_table_data_set_clone_row(row);
                    newrowstash->newrow = newrow;
                }
                netsnmp_oid_stash_add_data(stashp, suffix, suffix_len,
                                           newrowstash);
            } else {
                newrow = newrowstash->newrow;
            }
            /* all future SET data modification operations use this
               temp pointer */
            if (reqinfo->mode == MODE_SET_RESERVE1 ||
                reqinfo->mode == MODE_SET_RESERVE2)
                row = newrow;
        }

        if (row)
            data = (netsnmp_table_data_set_storage *) row->data;

        if (!row || !table_info || !data) {
            if (!MODE_IS_SET(reqinfo->mode)) {
                netsnmp_set_request_error(reqinfo, request, SNMP_ERR_NOSUCHNAME);
                continue;
            }
        }

      topsearch:
        data = netsnmp_table_data_set_find_column(data, table_info->colnum);

        switch(reqinfo->mode) {
            case MODE_GET:
            case MODE_GETNEXT:
            case MODE_GETBULK: /* XXXWWW */
                if (!data) {
                    netsnmp_table_row *newrow;
                    netsnmp_table_data_set_storage *newdata = NULL;
/*                    unsigned int column = table_info->colnum; */

                    while(table_info->colnum <= table_info->reg_info->max_column) {
                        if (table_info->colnum != table_info->colnum) {
                            /* start with a new row */
                            row = datatable->table->first_row;
                        }
                        for(newrow = row; newrow; newrow = newrow->next) {
                            newdata = (netsnmp_table_data_set_storage *) newrow->data;
                            if (newdata)
                                newdata =
                                    netsnmp_table_data_set_find_column(newdata,
                                                               table_info->colnum);
                            if (newdata) {
                                /* this is it */
                                data = newdata;
                                row = newrow;
                                goto done;
                            }
                        }
                        table_info->colnum++;
                    }
                }
          done:
                if (data && data->data.voidp)
                    netsnmp_table_data_build_result(reginfo, reqinfo, request, row,
                                            table_info->colnum,
                                            data->type,
                                            data->data.voidp, data->data_len);
                else {
                    /* deal with holes by going to the next data set
                       in the row or possibly onward to new columns */
                    if (reqinfo->mode == MODE_GETNEXT ||
                        reqinfo->mode == MODE_GETBULK) { /* XXXWWW */
                        if (row)
                            row = row->next;
                        if (row) {
                            data = (netsnmp_table_data_set_storage *) row->data;
                            goto topsearch;
                        }
                        if (table_info->colnum <=
                            table_info->reg_info->max_column) {
                            table_info->colnum++;
                            row = datatable->table->first_row;
                            data = (netsnmp_table_data_set_storage *) row->data;
                            goto topsearch;
                        }
                    }
                }
                break;

            case MODE_SET_RESERVE1:
                if (data) {
                    /* modify existing */
                    if (!data->writable) {
                        netsnmp_set_request_error(reqinfo, request,
                                          SNMP_ERR_NOTWRITABLE);
                    } else if (request->requestvb->type != data->type) {
                        netsnmp_set_request_error(reqinfo, request,
                                          SNMP_ERR_WRONGTYPE);
                    }
                } else {
                    /* now column definition found.  error out */
                    netsnmp_set_request_error(reqinfo, request,
                                              SNMP_ERR_NOTWRITABLE);
                }
                break;

            case MODE_SET_RESERVE2:
                /* modify row and set new value */
                memdup(&data->data.string, request->requestvb->val.string,
                       request->requestvb->val_len);
                if (!data->data.string) {
                    netsnmp_set_request_error(reqinfo, requests,
                                              SNMP_ERR_RESOURCEUNAVAILABLE);
                }
                data->data_len = request->requestvb->val_len;
                break;

            case MODE_SET_ACTION:
                
                /* install the new row.  Do this only *once* per row */
                if (newrowstash->state != STATE_ACTION) {
                    newrowstash->state = STATE_ACTION;
                    if (newrowstash->created)
                        netsnmp_table_dataset_add_row(datatable, newrow);
                    else
                        netsnmp_table_dataset_replace_row(datatable,
                                                          row, newrow);
                }
                break;
                
            case MODE_SET_UNDO:
                /* extract the new row, replace with the old or delete */
                if (newrowstash->state != STATE_UNDO) {
                    newrowstash->state = STATE_UNDO;
                    if (newrowstash->created)
                        netsnmp_table_dataset_delete_row(datatable, newrow);
                    else
                        netsnmp_table_dataset_replace_row(datatable,
                                                          newrow, row);
                }
                /* XXXWWW: free new row */
                
                break;

            case MODE_SET_COMMIT:
                /* XXXWWW: free replaced row */
                break;

            case MODE_SET_FREE:
                /* hmmm....  */
                /* nothing to do */
                break;
        }
    }

    if (handler->next && handler->next->access_method)
        netsnmp_call_next_handler(handler, reginfo, reqinfo, requests);
    return SNMP_ERR_NOERROR;
}
    
/** @internal */
void
netsnmp_config_parse_table_set(const char *token, char *line) 
{
    oid name[MAX_OID_LEN], table_name[MAX_OID_LEN];
    size_t name_length = MAX_OID_LEN, table_name_length = MAX_OID_LEN;
    struct tree *tp, *indexnode;
    netsnmp_table_data_set *table_set;
    struct index_list *index;
    unsigned int mincol = 0xffffff, maxcol = 0;
    data_set_tables *tables;
    u_char type;
    
    /* instatiate a fake table based on MIB information */
    if (!snmp_parse_oid(line, table_name, &table_name_length) ||
        (NULL == (tp = get_tree(table_name, table_name_length,
                                get_tree_head())))) {
        config_pwarn("can't instatiate table %s since I can't find mib information about it\n");
        return;
    }

    if (NULL == (tp = tp->child_list) ||
        NULL == tp->child_list) {
        config_pwarn("can't instatiate table since it doesn't appear to be a proper table\n");
        return;
    }

    table_set = netsnmp_create_table_data_set(line);

    /* loop through indexes and add types */
    for(index = tp->indexes; index; index = index->next) {
        if (!snmp_parse_oid(index->ilabel, name, &name_length) ||
            (NULL == (indexnode = get_tree(name, name_length, get_tree_head())))) {
            config_pwarn("can't instatiate table %s since I don't know anything about one index\n");
            return; /* xxx mem leak */
        }

        type = mib_to_asn_type(indexnode->type);
        if (type ==(u_char) -1) {
            config_pwarn("unknown index type");
            return; /* xxx mem leak */
        }
        if (index->isimplied) /* if implied, mark it as such */
            type |= ASN_PRIVATE;
            
        DEBUGMSGTL(("table_set_add_row","adding default index of type %d\n",
                    type));
        netsnmp_table_dataset_add_index(table_set, type);
    }

    /* loop through children and add each column info */
    for(tp = tp->child_list; tp; tp = tp->next_peer) {
        int canwrite = 0;
        type = mib_to_asn_type(tp->type);
        if (type == (u_char)-1) {
            config_pwarn("unknown column type");
            return; /* xxx mem leak */
        }
        
        DEBUGMSGTL(("table_set_add_row","adding column %d of type %d\n",
                    tp->subid, type));

        switch (tp->access) {
            case MIB_ACCESS_CREATE:
            case MIB_ACCESS_READWRITE:
            case MIB_ACCESS_WRITEONLY:
                canwrite = 1;
            case MIB_ACCESS_READONLY:
                DEBUGMSGTL(("table_set_add_row","adding column %d of type %d\n",
                            tp->subid, type));
                netsnmp_table_set_add_default_row(table_set, tp->subid, type, canwrite, NULL, 0);
                mincol = SNMP_MIN(mincol, tp->subid);
                maxcol = SNMP_MAX(maxcol, tp->subid);
                break;

            case MIB_ACCESS_NOACCESS:
            case MIB_ACCESS_NOTIFY:
                break;

            default:
                config_pwarn("unknown column access type");
                break;
        }
    }

    /* register the table */
    netsnmp_register_netsnmp_table_data_set(
        netsnmp_create_handler_registration(line, NULL, table_name, table_name_length,
                                    HANDLER_CAN_RWRITE),
        table_set, NULL);

    tables = SNMP_MALLOC_TYPEDEF(data_set_tables);
    tables->table_set = table_set;
    netsnmp_add_list_data(&auto_tables, netsnmp_create_data_list(line, tables, NULL));
}

/** @internal */
void
netsnmp_config_parse_add_row(const char *token, char *line) 
{
    char buf[SNMP_MAXBUF_MEDIUM];
    char tname[SNMP_MAXBUF_MEDIUM];
    size_t buf_size;

    data_set_tables *tables;
    netsnmp_variable_list *vb; /* containing only types */
    netsnmp_table_row *row;
    netsnmp_table_data_set_storage *dr;
    
    line = copy_nword(line, tname, SNMP_MAXBUF_MEDIUM);

    tables = (data_set_tables *) netsnmp_get_list_data(auto_tables, tname);
    if (!tables) {
        config_pwarn("Unknown table trying to add a row");
        return;
    }

    /* do the indexes first */
    row = netsnmp_create_table_data_row();

    for(vb = tables->table_set->table->indexes_template; vb;
        vb = vb->next_variable) {
        if (!line) {
            config_pwarn("missing an index value");
            return;
        }
        
        DEBUGMSGTL(("table_set_add_row","adding index of type %d\n", vb->type));
        buf_size = SNMP_MAXBUF_MEDIUM;
        line = read_config_read_memory(vb->type, line, buf, &buf_size);
        netsnmp_table_row_add_index(row, vb->type, buf, buf_size);
    }

    /* then do the data */
    for(dr = tables->table_set->default_row; dr; dr = dr->next) {
        if (!line) {
            config_pwarn("missing an data value\n");
            return;
        }
        
        buf_size = SNMP_MAXBUF_MEDIUM;
        line = read_config_read_memory(dr->type, line, buf, &buf_size);
        DEBUGMSGTL(("table_set_add_row","adding data at column %d of type %d\n", dr->column, dr->type));
        netsnmp_set_row_column(row, dr->column, dr->type, buf, buf_size);
        if (dr->writable)
            netsnmp_mark_row_column_writable(row, dr->column, 1); /* make writable */
    }
    netsnmp_table_data_add_row(tables->table_set->table, row);
}

/** adds an index to the table.  Call this repeatly for each index. */
inline void
netsnmp_table_dataset_add_index(netsnmp_table_data_set *table, u_char type) 
{
    netsnmp_table_data_add_index(table->table, type);
}

/** adds a new row to a dataset table */
inline void
netsnmp_table_dataset_add_row(netsnmp_table_data_set *table,
                              netsnmp_table_row *row)
{
    netsnmp_table_data_add_row(table->table, row);
}

/** adds a new row to a dataset table */
inline void
netsnmp_table_dataset_replace_row(netsnmp_table_data_set *table,
                                  netsnmp_table_row *origrow,
                                  netsnmp_table_row *newrow)
{
    netsnmp_table_data_replace_row(table->table, origrow, newrow);
}

/** deletes a row (and all it's data) from a dataset table */
inline void
netsnmp_table_dataset_delete_row(netsnmp_table_data_set *table,
                                 netsnmp_table_row *row)
{
    
    netsnmp_table_data_set_storage *datatmp;
    netsnmp_table_data_set_storage *data =
        (netsnmp_table_data_set_storage *)
        netsnmp_table_data_delete_row(table->table, row);

    while(data) {
        SNMP_FREE(data->data.voidp);
        datatmp = data->next;
        free(data);
        data = datatmp;
    }
}

/** adds multiple data column definitions to each row.  Functionally,
 *  this is a wrapper around calling netsnmp_table_set_add_default_row
 *  repeatedly for you.
 */
  void
#if HAVE_STDARG_H
netsnmp_table_set_multi_add_default_row(netsnmp_table_data_set *tset, ...)
#else
netsnmp_table_set_multi_add_default_row(va_dcl)
  va_dcl
#endif
{
  va_list debugargs;
  unsigned int column;
  int type, writable;
  void *data;
  size_t data_len;
  
#if HAVE_STDARG_H
  va_start(debugargs,tset);
#else
  netsnmp_table_data_set *tset;
  
  va_start(debugargs);
  tset = va_arg(debugargs, netsnmp_table_data_set *);
#endif

  while((column = va_arg(debugargs, unsigned int)) != 0) {
      type = va_arg(debugargs, int);
      writable = va_arg(debugargs, int);
      data = va_arg(debugargs, void *);
      data_len = va_arg(debugargs, size_t);
      netsnmp_table_set_add_default_row(tset, column, type, writable,
                                        data, data_len);
  }

  va_end(debugargs);
}

/* @} */
