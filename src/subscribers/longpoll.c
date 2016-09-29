#include <nchan_module.h>
#include <subscribers/common.h>
#define DEBUG_LEVEL NGX_LOG_WARN
//#define DEBUG_LEVEL NGX_LOG_DEBUG
#define DBG(fmt, arg...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "SUB:LONGPOLL:" fmt, ##arg)
#define ERR(fmt, arg...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "SUB:LONGPOLL:" fmt, ##arg)
#include <assert.h>
#include "longpoll-private.h"

#include <store/memory/store.h>

void memstore_fakeprocess_push(ngx_int_t slot);
void memstore_fakeprocess_push_random(void);
void memstore_fakeprocess_pop(void);
ngx_int_t memstore_slot(void);

static const subscriber_t new_longpoll_sub;

static void empty_handler(void) { }

ngx_int_t longpoll_subscriber_destroy(subscriber_t *sub);

static int maybe_send_unsubscribe_upstream_request(full_subscriber_t *fsub, ngx_int_t status) {
  if(fsub->data.was_enqueued && fsub->sub.cf->unsubscribe_request_url) {
    nchan_request_ctx_t           *ctx = ngx_http_get_module_ctx(fsub->sub.request, ngx_nchan_module);
    ctx->unsubscribe_request_finalize_code = status;
    fsub->sub.request->main->blocked = 1;
    return 1;
  }
  return 0;
}

static void longpoll_finalize_request(full_subscriber_t *fsub, ngx_int_t status) {
  if(maybe_send_unsubscribe_upstream_request(fsub, status)) {
    status = NGX_HTTP_CLIENT_CLOSED_REQUEST;
  }
  ngx_http_finalize_request(fsub->sub.request, status);
}

static void sudden_abort_handler(full_subscriber_t *fsub) {
#if FAKESHARD
  memstore_fakeprocess_push(fsub->sub.owner);
#endif
  if(fsub->sub.cf->unsubscribe_request_url) {
    nchan_request_ctx_t           *ctx = ngx_http_get_module_ctx(fsub->sub.request, ngx_nchan_module);
    ctx->block_on_unsubscribe_request = 1;
    ctx->unsubscribe_request_finalize_code = NGX_HTTP_CLIENT_CLOSED_REQUEST;
  }
  fsub->sub.status = DEAD;
  fsub->data.finalize_request = 0;
  fsub->sub.fn->dequeue(&fsub->sub);
  longpoll_subscriber_destroy(&fsub->sub);
#if FAKESHARD
  memstore_fakeprocess_pop();
#endif
}

//void verify_unique_response(ngx_str_t *uri, nchan_msg_id_t *msgid, nchan_msg_t *msg, subscriber_t *sub);

subscriber_t *longpoll_subscriber_create(ngx_http_request_t *r, nchan_msg_id_t *msg_id) {
  DBG("create for req %p", r);
  full_subscriber_t      *fsub;
  nchan_request_ctx_t  *ctx = ngx_http_get_module_ctx(r, ngx_nchan_module);
  
  //TODO: allocate from pool (but not the request's pool)
  if((fsub = ngx_alloc(sizeof(*fsub), ngx_cycle->log)) == NULL) {
    ERR("Unable to allocate");
    assert(0);
    return NULL;
  }
  
  nchan_subscriber_init(&fsub->sub, &new_longpoll_sub, r, msg_id);
  fsub->privdata = NULL;
  fsub->data.cln = NULL;
  fsub->data.finalize_request = 1;
  fsub->data.holding = 0;
  fsub->data.act_as_intervalpoll = 0;
  
  nchan_subscriber_init_timeout_timer(&fsub->sub, &fsub->data.timeout_ev);
  
  fsub->data.dequeue_handler = empty_handler;
  fsub->data.dequeue_handler_data = NULL;
  
  fsub->data.already_responded = 0;
  fsub->data.awaiting_destruction = 0;
  fsub->data.was_enqueued = 0;
  
  if(fsub->sub.cf->longpoll_multimsg) {
    fsub->sub.dequeue_after_response = 0;
    ctx->bcp = ngx_palloc(r->pool, sizeof(nchan_bufchain_pool_t));
    nchan_bufchain_pool_init(ctx->bcp, r->pool);
  }
  
  ctx->block_on_unsubscribe_request = 1;
  ctx->unsubscribe_request_finalize_code = NGX_DECLINED;
  
  fsub->data.multimsg_first = NULL;
  fsub->data.multimsg_last = NULL;
  
#if NCHAN_SUBSCRIBER_LEAK_DEBUG
  subscriber_debug_add(&fsub->sub);
  //set debug label
  fsub->sub.lbl = ngx_calloc(r->uri.len+1, ngx_cycle->log);
  ngx_memcpy(fsub->sub.lbl, r->uri.data, r->uri.len);
#endif
  
  //http request sudden close cleanup
  if((fsub->data.cln = ngx_http_cleanup_add(r, 0)) == NULL) {
    ERR("Unable to add request cleanup for longpoll subscriber");
    assert(0);
    return NULL;
  }
  fsub->data.cln->data = fsub;
  fsub->data.cln->handler = (ngx_http_cleanup_pt )sudden_abort_handler;
  DBG("%p created for request %p", &fsub->sub, r);
  
  
  return &fsub->sub;
}

ngx_int_t longpoll_subscriber_destroy(subscriber_t *sub) {
  full_subscriber_t   *fsub = (full_subscriber_t  *)sub;
  
  if(sub->reserved > 0) {
    DBG("%p not ready to destroy (reserved for %i) for req %p", sub, sub->reserved, fsub->sub.request);
    fsub->data.awaiting_destruction = 1;
  }
  else {
    DBG("%p destroy for req %p", sub, fsub->sub.request);
    nchan_free_msg_id(&fsub->sub.last_msgid);
    assert(sub->status == DEAD);
#if NCHAN_SUBSCRIBER_LEAK_DEBUG
    subscriber_debug_remove(sub);
    ngx_free(sub->lbl);
    ngx_memset(fsub, 0xB9, sizeof(*fsub)); //debug
#endif
    ngx_free(fsub);
  }
  return NGX_OK;
}

static void ensure_request_hold(full_subscriber_t *fsub) {
  if(fsub->data.holding == 0) {
    DBG("hodl request %p", fsub->sub.request);
    fsub->data.holding = 1;
    fsub->sub.request->read_event_handler = ngx_http_test_reading;
    fsub->sub.request->write_event_handler = ngx_http_request_empty_handler;
    fsub->sub.request->main->count++; //this is the right way to hold and finalize the request... maybe
  }
}

static ngx_int_t longpoll_reserve(subscriber_t *self) {
  full_subscriber_t  *fsub = (full_subscriber_t  *)self;
  ensure_request_hold(fsub);
  self->reserved++;
  DBG("%p reserve for req %p, reservations: %i", self, fsub->sub.request, self->reserved);
  return NGX_OK;
}
static ngx_int_t longpoll_release(subscriber_t *self, uint8_t nodestroy) {
  full_subscriber_t  *fsub = (full_subscriber_t  *)self;
  assert(self->reserved > 0);
  self->reserved--;
  DBG("%p release for req %p. reservations: %i", self, fsub->sub.request, self->reserved);
  if(nodestroy == 0 && fsub->data.awaiting_destruction == 1 && self->reserved == 0) {
    longpoll_subscriber_destroy(self);
    return NGX_ABORT;
  }
  else {
    return NGX_OK;
  }
}

ngx_int_t longpoll_enqueue(subscriber_t *self) {
  full_subscriber_t  *fsub = (full_subscriber_t  *)self;
  assert(fsub->sub.enqueued == 0);
  DBG("%p enqueue", self);
  
  fsub->data.finalize_request = 1;
  
  fsub->sub.enqueued = 1;
  ensure_request_hold(fsub);
  if(self->cf->subscriber_timeout > 0) {
    //add timeout timer
    ngx_add_timer(&fsub->data.timeout_ev, self->cf->subscriber_timeout * 1000);
  }
  
  return NGX_OK;
}

static ngx_int_t longpoll_dequeue(subscriber_t *self) {
  full_subscriber_t    *fsub = (full_subscriber_t  *)self;
  nchan_request_ctx_t  *ctx = ngx_http_get_module_ctx(fsub->sub.request, ngx_nchan_module);
  if(fsub->data.timeout_ev.timer_set) {
    ngx_del_timer(&fsub->data.timeout_ev);
  }
  DBG("%p dequeue", self);
  
  fsub->data.dequeue_handler(self, fsub->data.dequeue_handler_data);

  self->enqueued = 0;
  
  ctx->sub = NULL;
  
  if(fsub->data.finalize_request) {
    DBG("finalize request %p %i", fsub->sub.request);
    
    longpoll_finalize_request(fsub, NGX_OK);
    self->status = DEAD;
  }
  
  /*if(self->destroy_after_dequeue) {
    longpoll_subscriber_destroy(self);
  }*/
  return NGX_OK;
}

static ngx_int_t dequeue_maybe(subscriber_t *self) {
  if(self->dequeue_after_response) {
    self->fn->dequeue(self);
  }
  return NGX_OK;
}

static ngx_int_t abort_response(subscriber_t *sub, char *errmsg) {
  ERR("abort! %s", errmsg ? errmsg : "unknown error");
  dequeue_maybe(sub);
  return NGX_ERROR;
}

static ngx_int_t longpoll_multipart_add(full_subscriber_t *fsub, nchan_msg_t *msg, char **err) {
  
  nchan_longpoll_multimsg_t     *mmsg;
  
  if((mmsg = ngx_palloc(fsub->sub.request->pool, sizeof(*mmsg))) == NULL) {
    *err = "can't allocate multipart msg link";
    return NGX_ERROR;
  }
  
  if(msg->shared) {
    msg_reserve(msg, "longpoll multipart");
  }
  else if(msg->id.tagcount > 1) {
    //msg from a multiplexed channel
    assert(!msg->shared && !msg->temp_allocd);
    nchan_msg_copy_t *cmsg;
    if((cmsg = ngx_palloc(fsub->sub.request->pool, sizeof(*cmsg))) == NULL) {
      *err = "can't allocate msgcopy for message from multiplexed channel";
      return NGX_ERROR;
      
    }
    //  multiplexed channel message should have been created as a nchan_msg_copy_t
    *cmsg = *(nchan_msg_copy_t *)msg;
    
    cmsg->copy.temp_allocd = 1;
    
    assert(cmsg->original->shared);
    msg_reserve(cmsg->original, "longpoll multipart for multiplexed channel");
    msg = &cmsg->copy;
  }
  else {
    assert(0); //this is not yet an expected scenario;
  }
  
  mmsg->msg = msg;
  mmsg->next = NULL;
  if(fsub->data.multimsg_first == NULL) {
    fsub->data.multimsg_first = mmsg;
  }
  if(fsub->data.multimsg_last) {
    fsub->data.multimsg_last->next = mmsg;
  }
  fsub->data.multimsg_last = mmsg;
  
  return NGX_OK;
}

static ngx_int_t longpoll_respond_message(subscriber_t *self, nchan_msg_t *msg) {
  full_subscriber_t  *fsub = (full_subscriber_t  *)self;
  ngx_int_t                  rc;
  char                      *err = NULL;
  ngx_http_request_t        *r = fsub->sub.request;
  nchan_request_ctx_t       *ctx = ngx_http_get_module_ctx(r, ngx_nchan_module);
  nchan_loc_conf_t          *cf = fsub->sub.cf;
  
  DBG("%p respond req %p msg %p", self, r, msg);
  
  ctx->prev_msg_id = self->last_msgid;
  update_subscriber_last_msg_id(self, msg);
  ctx->msg_id = self->last_msgid;

  //verify_unique_response(&fsub->data.request->uri, &self->last_msgid, msg, self);
  if(fsub->data.timeout_ev.timer_set) {
    ngx_del_timer(&fsub->data.timeout_ev);
  }
  if(!cf->longpoll_multimsg) {
    //disable abort handler
    fsub->data.cln->handler = empty_handler;
    
    assert(fsub->data.already_responded != 1);
    fsub->data.already_responded = 1;
    if((rc = nchan_respond_msg(r, msg, &self->last_msgid, 0, &err)) != NGX_OK) {
      return abort_response(self, err);
    }
  }
  else {
    if((rc = longpoll_multipart_add(fsub, msg, &err)) != NGX_OK) {
      return abort_response(self, err);
    }
  }
  dequeue_maybe(self);
  return rc;
}

static void multipart_request_cleanup_handler(full_subscriber_t *fsub) {
  nchan_longpoll_multimsg_t    *cur;
  nchan_msg_copy_t             *cmsg;
  nchan_longpoll_multimsg_t    *first = fsub->data.multimsg_first;
  for(cur = first; cur != NULL; cur = cur->next) {
    if(cur->msg->shared) {
      msg_release(cur->msg, "longpoll multipart");
    }
    else if(cur->msg->id.tagcount > 1) {
      assert(!cur->msg->shared && cur->msg->temp_allocd);
      // multiplexed channel message should have been created as a nchan_msg_copy_t
      cmsg = (nchan_msg_copy_t *)cur->msg;
      
      assert(cmsg->original->shared);
      msg_release(cmsg->original, "longpoll multipart for multiplexed channel");
    }
    else {
      assert(0);
    }
  }
}

static ngx_int_t longpoll_multipart_respond(full_subscriber_t *fsub) {
  ngx_http_request_t    *r = fsub->sub.request;
  nchan_request_ctx_t   *ctx = ngx_http_get_module_ctx(r, ngx_nchan_module);
  char                  *err;
  ngx_int_t              rc;
  u_char                *char_boundary = NULL;
  u_char                *char_boundary_last;
  
  ngx_buf_t              boundary[3]; //first, mid, and last boundary
  ngx_buf_t              newline_buf;
  ngx_chain_t           *chain, *first_chain = NULL, *last_chain = NULL;
  ngx_buf_t             *buf;
  ngx_buf_t              double_newline_buf;
  ngx_str_t             *content_type;
  size_t                 size = 0;
  nchan_loc_conf_t      *cf = fsub->sub.cf;
  int                    use_raw_stream_separator = cf->longpoll_multimsg_use_raw_stream_separator;
  nchan_buf_and_chain_t *bc;
  
  nchan_longpoll_multimsg_t *first, *cur;
  
  //disable abort handler
  fsub->data.cln->handler = empty_handler;
  
  first = fsub->data.multimsg_first;
  
  fsub->sub.dequeue_after_response = 1;
  
  //cleanup to release msgs
  fsub->data.cln = ngx_http_cleanup_add(fsub->sub.request, 0);
  fsub->data.cln->data = fsub;
  fsub->data.cln->handler = (ngx_http_cleanup_pt )multipart_request_cleanup_handler;
  
  if(fsub->data.multimsg_first == fsub->data.multimsg_last) {
    //just one message.
    if((rc = nchan_respond_msg(r, fsub->data.multimsg_first->msg, &fsub->sub.last_msgid, 0, &err)) != NGX_OK) {
      return abort_response(&fsub->sub, err);
    }
    return NGX_OK;
  }
  
  //multi messages
  if(!use_raw_stream_separator) {
    nchan_request_set_content_type_multipart_boundary_header(r, ctx);
    char_boundary = ngx_palloc(r->pool, 50);
    char_boundary_last = ngx_snprintf(char_boundary, 50, ("\r\n--%V--\r\n"), nchan_request_multipart_boundary(r, ctx));
    
    ngx_init_set_membuf_char(&double_newline_buf, "\r\n\r\n");
    
    //set up the boundaries
    ngx_init_set_membuf(&boundary[0], &char_boundary[2], &char_boundary_last[-4]);
    ngx_init_set_membuf(&boundary[1], &char_boundary[0], &char_boundary_last[-4]);
    ngx_init_set_membuf(&boundary[2], &char_boundary[0], char_boundary_last);
    
    ngx_init_set_membuf_char(&newline_buf, "\n");
  }
  
  int n=0;
  
  for(cur = first; cur != NULL; cur = cur->next) {
    bc = nchan_bufchain_pool_reserve(ctx->bcp, 4);
    chain = &bc->chain;
    n++;
    
    if(last_chain) {
      last_chain->next = chain;
    }
    if(!first_chain) {
      first_chain = chain;
    }
    if(!use_raw_stream_separator) {
      // each buffer needs to be unique for the purpose of dealing with nginx output guts
      // (something about max. 64 iovecs per write call and counting the number of bytes already sent)
      *chain->buf = cur == first ? boundary[0] : boundary[1];
      size += ngx_buf_size((chain->buf));
      chain = chain->next;
      
      content_type = &cur->msg->content_type;
      buf = chain->buf;
      if (content_type->data != NULL) {
        u_char    *char_cur = ngx_pcalloc(r->pool, content_type->len + 25);
        ngx_init_set_membuf(buf, char_cur, ngx_snprintf(char_cur, content_type->len + 25, "\r\nContent-Type: %V\r\n\r\n", content_type));
      }
      else {
        *buf = double_newline_buf;
      }
      size += ngx_buf_size(buf);
      chain = chain->next;
    }
      
    if(ngx_buf_size(cur->msg->buf) > 0) {
      buf = chain->buf;
      *buf = *cur->msg->buf;
      
      if(buf->file) {
        ngx_file_t  *file_copy = nchan_bufchain_pool_reserve_file(ctx->bcp);
        nchan_msg_buf_open_fd_if_needed(buf, file_copy, NULL);
      }
      buf->last_buf = 0;
      size += ngx_buf_size(buf);
    }
    
    if(use_raw_stream_separator) {
      chain = chain->next;
      ngx_init_set_membuf_str(chain->buf, &cf->subscriber_http_raw_stream_separator);
      size += ngx_buf_size((chain->buf));
    }
    else {
      if(cur->next == NULL) {
        chain = chain->next;
        chain->buf = &boundary[2];
        size += ngx_buf_size((chain->buf));
      }
    }
    last_chain = chain;
  }
    
  buf = last_chain->buf;
  buf->last_buf = 1;
  buf->last_in_chain = 1;
  buf->flush = 1;
  last_chain->next = NULL;
    
  r->headers_out.status = NGX_HTTP_OK;
  r->headers_out.content_length_n = size;
  nchan_set_msgid_http_response_headers(r, ctx, &fsub->data.multimsg_last->msg->id);
  nchan_include_access_control_if_needed(r, ctx);
  ngx_http_send_header(r);
  nchan_output_filter(r, first_chain);
  
  
  return NGX_OK;
}

static ngx_int_t longpoll_respond_status(subscriber_t *self, ngx_int_t status_code, const ngx_str_t *status_line) {
  
  full_subscriber_t     *fsub = (full_subscriber_t *)self;
  ngx_http_request_t    *r = fsub->sub.request;
  nchan_loc_conf_t      *cf = fsub->sub.cf;
  
  //DBG("%p got status %i", self, status_code);
  
  if(fsub->data.act_as_intervalpoll) {
    if(status_code == NGX_HTTP_NO_CONTENT || status_code == NGX_HTTP_NOT_MODIFIED || status_code == NGX_HTTP_NOT_FOUND ) {
      status_code = NGX_HTTP_NOT_MODIFIED;
    }
  }
  else if(status_code == NGX_HTTP_NO_CONTENT || (status_code == NGX_HTTP_NOT_MODIFIED && !status_line)) {
    if(cf->longpoll_multimsg) {
      if(fsub->data.multimsg_first != NULL) {
        longpoll_multipart_respond(fsub);
        dequeue_maybe(self);
      }
      return NGX_OK;
    }
    else { 
      //don't care, ignore
      return NGX_OK;
    }
  }
  
  
  
  DBG("%p respond req %p status %i", self, r, status_code);
  
  if(fsub->sub.type == LONGPOLL && fsub->sub.cf->longpoll_multimsg) {
    fsub->sub.dequeue_after_response = 1;
  }
  
  nchan_set_msgid_http_response_headers(r, NULL, &self->last_msgid);
  
  //disable abort handler
  fsub->data.cln->handler = empty_handler;
  
  nchan_respond_status(r, status_code, status_line, 0);

  dequeue_maybe(self);
  return NGX_OK;
}

ngx_int_t subscriber_respond_unqueued_status(full_subscriber_t *fsub, ngx_int_t status_code, const ngx_str_t *status_line) {
  ngx_http_request_t     *r = fsub->sub.request;
  fsub->data.cln->handler = (ngx_http_cleanup_pt )empty_handler;
  fsub->data.finalize_request = 0;
  fsub->sub.status = DEAD;
  fsub->sub.fn->dequeue(&fsub->sub);
  return nchan_respond_status(r, status_code, status_line, 1);
}

void subscriber_maybe_dequeue_after_status_response(full_subscriber_t *fsub, ngx_int_t status_code) {
  if((status_code >=400 && status_code < 600) || status_code == NGX_HTTP_NOT_MODIFIED) {
    fsub->data.cln->handler = (ngx_http_cleanup_pt )empty_handler;
    fsub->sub.request->keepalive=0;
    if(!fsub->sub.cf->unsubscribe_request_url) {
      fsub->data.finalize_request=1;
    }
    fsub->sub.fn->dequeue(&fsub->sub);
  }
}

static void request_cleanup_handler(subscriber_t *sub) {

}


static ngx_int_t longpoll_set_dequeue_callback(subscriber_t *self, subscriber_callback_pt cb, void *privdata) {
  full_subscriber_t  *fsub = (full_subscriber_t  *)self;
  if(fsub->data.cln == NULL) {
    fsub->data.cln = ngx_http_cleanup_add(fsub->sub.request, 0);
    fsub->data.cln->data = self;
    fsub->data.cln->handler = (ngx_http_cleanup_pt )request_cleanup_handler;
  }
  fsub->data.dequeue_handler = cb;
  fsub->data.dequeue_handler_data = privdata;
  return NGX_OK;
}

static const subscriber_fn_t longpoll_fn = {
  &longpoll_enqueue,
  &longpoll_dequeue,
  &longpoll_respond_message,
  &longpoll_respond_status,
  &longpoll_set_dequeue_callback,
  &longpoll_reserve,
  &longpoll_release,
  &nchan_subscriber_empty_notify,
  &nchan_subscriber_authorize_subscribe_request
};

static ngx_str_t  sub_name = ngx_string("longpoll");

static const subscriber_t new_longpoll_sub = {
  &sub_name,
  LONGPOLL,
  &longpoll_fn,
  UNKNOWN,
  NCHAN_ZERO_MSGID,
  NULL,
  NULL,
  0, //reservations
  1, //deque after response
  1, //destroy after dequeue
  0, //enqueued
};
