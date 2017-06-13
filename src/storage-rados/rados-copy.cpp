/* Copyright (c) 2017 Tallence AG and the authors, see the included COPYING file */

extern "C" {

#include "lib.h"
#include "typeof-def.h"
#include "istream.h"
#include "mail-storage.h"
#include "mail-copy.h"

#include "rados-storage-local.h"
#include "debug-helper.h"
}

int rados_mail_storage_copy(struct mail_save_context *ctx, struct mail *mail);

int rados_mail_copy(struct mail_save_context *_ctx, struct mail *mail) {
  FUNC_START();
  struct rados_save_context *ctx = (struct rados_save_context *)_ctx;

  debug_print_mail(mail, "rados_mail_copy", NULL);
  debug_print_mail_save_context(_ctx, "rados_mail_copy", NULL);

  int ret = rados_mail_storage_copy(_ctx, mail);
  FUNC_END();
  return ret;
}

static void rados_mail_copy_set_failed(struct mail_save_context *ctx, struct mail *mail, const char *func) {
  const char *errstr;
  enum mail_error error;

  if (ctx->transaction->box->storage == mail->box->storage)
    return;

  errstr = mail_storage_get_last_error(mail->box->storage, &error);
  mail_storage_set_error(ctx->transaction->box->storage, error, t_strdup_printf("%s (%s)", errstr, func));
}

int rados_mail_save_copy_default_metadata(struct mail_save_context *ctx, struct mail *mail) {
  FUNC_START();
  const char *from_envelope, *guid;
  time_t received_date;

  if (ctx->data.received_date == (time_t)-1) {
    if (mail_get_received_date(mail, &received_date) < 0) {
      rados_mail_copy_set_failed(ctx, mail, "received-date");
      FUNC_END_RET("ret == -1, mail_get_received_date failed");
      return -1;
    }
    mailbox_save_set_received_date(ctx, received_date, 0);
  }
  if (ctx->data.from_envelope == NULL) {
    if (mail_get_special(mail, MAIL_FETCH_FROM_ENVELOPE, &from_envelope) < 0) {
      rados_mail_copy_set_failed(ctx, mail, "from-envelope");
      FUNC_END_RET("ret == -1, mail_get_special envelope failed");
      return -1;
    }
    if (*from_envelope != '\0')
      mailbox_save_set_from_envelope(ctx, from_envelope);
  }
  if (ctx->data.guid == NULL) {
    if (mail_get_special(mail, MAIL_FETCH_GUID, &guid) < 0) {
      rados_mail_copy_set_failed(ctx, mail, "guid");
      FUNC_END_RET("ret == -1, mail_get_special guid failed");
      return -1;
    }
    if (*guid != '\0')
      mailbox_save_set_guid(ctx, guid);
  }
  FUNC_END();
  return 0;
}

static int rados_mail_storage_try_copy(struct mail_save_context **_ctx, struct mail *mail) {
  FUNC_START();
  struct mail_save_context *ctx = *_ctx;
  struct mail_private *pmail = (struct mail_private *)mail;
  struct istream *input;

  ctx->copying_via_save = TRUE;

  /* we need to open the file in any case. caching metadata is unlikely
     to help anything. */
  pmail->v.set_uid_cache_updates(mail, TRUE);

  if (mail_get_stream_because(mail, NULL, NULL, "copying", &input) < 0) {
    rados_mail_copy_set_failed(ctx, mail, "stream");
    FUNC_END_RET("ret == -1, mail_get_stream_because failed");
    return -1;
  }

  if (rados_mail_save_copy_default_metadata(ctx, mail) < 0) {
    FUNC_END_RET("ret == -1, mail_save_copy_default_metadata failed");
    return -1;
  }

  if (mailbox_save_begin(_ctx, input) < 0) {
    FUNC_END_RET("ret == -1, mailbox_save_begin failed");
    return -1;
  }

  ssize_t ret;
  do {
    if (mailbox_save_continue(ctx) < 0)
      break;
    ret = i_stream_read(input);
    i_assert(ret != 0);
  } while (ret != -1);

  if (input->stream_errno != 0) {
    mail_storage_set_critical(ctx->transaction->box->storage, "copy: i_stream_read(%s) failed: %s",
                              i_stream_get_name(input), i_stream_get_error(input));
    FUNC_END_RET("ret == -1, input->stream_errno != 0");
    return -1;
  }
  FUNC_END();
  return 0;
}

int rados_mail_storage_copy(struct mail_save_context *ctx, struct mail *mail) {
  FUNC_START();
  i_assert(ctx->copying_or_moving);

  if (ctx->data.keywords != NULL) {
    /* keywords gets unreferenced twice: first in
       mailbox_save_cancel()/_finish() and second time in
       mailbox_copy(). */
    mailbox_keywords_ref(ctx->data.keywords);
  }

  if (rados_mail_storage_try_copy(&ctx, mail) < 0) {
    if (ctx != NULL)
      mailbox_save_cancel(&ctx);
    FUNC_END_RET("ret == -1, rados_mail_storage_try_copy failed");
    return -1;
  }
  FUNC_END();
  return mailbox_save_finish(&ctx);
}