// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Copyright (c) 2017 Tallence AG and the authors
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#include "rmb.h"
#include <stdarg.h> /* va_list, va_start, va_arg, va_end */
#include <errno.h>
#include <stdlib.h>

#include <iostream>
#include <map>
#include <vector>
#include <string>
#include <sstream>

#include <algorithm>  // std::sort

#include <limits>

#include "../../rados-cluster.h"
#include "../../rados-cluster-impl.h"
#include "../../rados-storage.h"
#include "../../rados-storage-impl.h"
#include "rados-mail-object.h"
#include "ls_cmd_parser.h"
#include "mailbox_tools.h"
#include "rados-util.h"
#include "rados-namespace-manager.h"
#include "rados-dovecot-ceph-cfg.h"
#include "rados-dovecot-ceph-cfg-impl.h"

static void argv_to_vec(int argc, const char **argv, std::vector<const char *> *args) {
  args->insert(args->end(), argv + 1, argv + argc);
}

static void dashes_to_underscores(const char *input, char *output) {
  char c = 0;
  char *o = output;
  const char *i = input;
  // first two characters are copied as-is
  *o = *i++;
  if (*o++ == '\0')
    return;
  *o = *i++;
  if (*o++ == '\0')
    return;
  for (; ((c = *i)); ++i) {
    if (c == '=') {
      strcpy(o, i);
      return;
    }
    if (c == '-')
      *o++ = '_';
    else
      *o++ = c;
  }
  *o++ = '\0';
}
bool ceph_argparse_flag(std::vector<const char *> &args, std::vector<const char *>::iterator &i, ...) {
  const char *first = *i;
  char tmp[strlen(first) + 1];
  dashes_to_underscores(first, tmp);
  first = tmp;
  va_list ap;

  va_start(ap, i);
  while (1) {
    const char *a = va_arg(ap, char *);
    if (a == NULL) {
      va_end(ap);
      return false;
    }
    char a2[strlen(a) + 1];
    dashes_to_underscores(a, a2);
    if (strcmp(a2, first) == 0) {
      i = args.erase(i);
      va_end(ap);
      return true;
    }
  }
}
static int va_ceph_argparse_witharg(std::vector<const char *> *args, std::vector<const char *>::iterator *i,
                                    std::string *ret, std::ostream &oss, va_list ap) {
  const char *first = *(*i);
  char tmp[strlen(first) + 1];
  dashes_to_underscores(first, tmp);
  first = tmp;

  // does this argument match any of the possibilities?
  while (1) {
    const char *a = va_arg(ap, char *);
    if (a == NULL)
      return 0;

    int strlen_a = strlen(a);
    char a2[strlen_a + 1];

    dashes_to_underscores(a, a2);
    if (strncmp(a2, first, strlen(a2)) == 0) {
      if (first[strlen_a] == '=') {
        *ret = first + strlen_a + 1;
        *i = args->erase(*i);
        return 1;
      } else if (first[strlen_a] == '\0') {
        // find second part (or not)
        if (*i + 1 == args->end()) {
          oss << "Option " << *(*i) << " requires an argument." << std::endl;
          *i = args->erase(*i);
          return -EINVAL;
        }
        *i = args->erase(*i);
        *ret = *(*i);
        *i = args->erase(*i);
        return 1;
      }
    }
  }
}

static bool ceph_argparse_witharg(std::vector<const char *> *args, std::vector<const char *>::iterator *i,
                                  std::string *ret, ...) {
  int r;
  va_list ap;
  va_start(ap, ret);
  r = va_ceph_argparse_witharg(args, i, ret, std::cerr, ap);
  va_end(ap);
  if (r < 0)
    _exit(1);
  return r != 0;
}

/** Once we see a standalone double dash, '--', we should remove it and stop
 * looking for any other options and flags. */
static bool ceph_argparse_double_dash(std::vector<const char *> *args, std::vector<const char *>::iterator *i) {
  if (strcmp(*(*i), "--") == 0) {
    *i = args->erase(*i);
    return true;
  }
  return false;
}

static void usage(std::ostream &out) {
  out << "usage: rmb [options] [commands]\n"
         "   -p pool\n"
         "        pool where mail data is saved, if not given mail_storage is used\n"
         "   -N namespace e.g. dovecot user name\n"
         "        specify the namespace/user to use for the mails\n"
         "   -O path to store the boxes. If not given, $HOME/rmb is used\n"
         "   lspools  list pools\n "
         "   -help print this information\n"
         "\n"
         "\nMAIL COMMANDS\n"
         "    ls     -   list all mails and mailbox statistic\n"
         "           all list all mails and mailbox statistic\n"
         "           <XATTR><OP><VALUE> e.g. U=7, \"U<7\", \"U>7\"\n"
         "                      <VALUE> e.g. R= %Y-%m-%d %H:%M:%S (\"R=2017-08-22 14:30\")\n"
         "                      <OP> =,>,< for strings only = is supported.\n"
         "    get     - download mails to file\n"
         "            <XATTR><OP><VALUE> e.g. U=7, \"U<7\", \"U>7\"\n"
         "                      <VALUE> e.g. R= %Y-%m-%d %H:%M:%S (\"R=2017-08-22 14:30\")\n"
         "                      <OP> =,>,< for strings only = is supported.\n"
         "    set     oid XATTR value e.g. U 1 B INBOX R \"2017-08-22 14:30\"\n"
         "    sort    uid, recv_date, save_date, phy_size\n"
         "\nMAILBOX COMMANDS\n"
         "    ls     mb  list all mailboxes\n"
         "\nCONFIGURATION COMMANDS\n"
         "    -cfg -U key=value [-obj <objectname>] -yes-i-really-really-mean-it \n"
         "                                          sets the configuration value\n"
         "                                          e.g. generated_namespace=true|false\n"
         "    -cfg -C [-obj <objectnam>]            create the default configuration\n"
         "    -cfg ls - [-obj <objectname>]         print the current configuation"

         "\n";
}

static void usage_exit() {
  usage(std::cerr);
  exit(1);
}

static void query_mail_storage(std::vector<librmb::RadosMailObject *> *mail_objects, librmb::CmdLineParser *parser,
                               bool download, librmb::RadosStorage *storage) {
  std::map<std::string, librmb::RadosMailBox *> mailbox;

  for (std::vector<librmb::RadosMailObject *>::iterator it = mail_objects->begin(); it != mail_objects->end(); ++it) {
    std::string mailbox_key = std::string(1, static_cast<char>(librmb::RBOX_METADATA_MAILBOX_GUID));
    std::string mailbox_guid = (*it)->get_metadata(mailbox_key);
    std::string mailbox_orig_name_key = std::string(1, static_cast<char>(librmb::RBOX_METADATA_ORIG_MAILBOX));
    std::string mailbox_orig_name = (*it)->get_metadata(mailbox_orig_name_key);

    if (parser->contains_key(mailbox_key)) {
      librmb::Predicate *p = parser->get_predicate(mailbox_key);
      if (!p->eval(mailbox_guid)) {
        continue;
      }
    }
    if (mailbox.count(mailbox_guid) > 0) {
      mailbox[mailbox_guid]->add_mail((*it));
      mailbox[mailbox_guid]->add_to_mailbox_size((*it)->get_mail_size());
    } else {
      mailbox[mailbox_guid] = new librmb::RadosMailBox(mailbox_guid, 1, mailbox_orig_name);
      mailbox[mailbox_guid]->set_xattr_filter(parser);
      mailbox[mailbox_guid]->add_mail((*it));
      mailbox[mailbox_guid]->add_to_mailbox_size((*it)->get_mail_size());
    }
  }
  std::cout << "mailbox_count: " << mailbox.size() << std::endl;

  {
    for (std::map<std::string, librmb::RadosMailBox *>::iterator it = mailbox.begin(); it != mailbox.end(); ++it) {
      if (it->second->get_mail_count() == 0) {
        continue;
      }
      std::cout << it->second->to_string() << std::endl;

      if (download) {
        librmb::MailboxTools tools(it->second, parser->get_output_dir());
        if (tools.init_mailbox_dir() < 0) {
          std::cout << " error initializing output dir : " << parser->get_output_dir() << std::endl;
          break;
        }

        for (std::vector<librmb::RadosMailObject *>::iterator it_mail = it->second->get_mails().begin();
             it_mail != it->second->get_mails().end(); ++it_mail) {
          const std::string oid = (*it_mail)->get_oid();

          time_t pm_time;
          uint64_t size_r = 0;
          storage->stat_mail(oid, &size_r, &pm_time);

          librados::bufferlist buffer;

          char *mail_buffer = new char[size_r + 1];
          (*it_mail)->set_mail_buffer(mail_buffer);

          (*it_mail)->set_mail_size(size_r);
          int read = storage->read_mail(oid, &buffer);
          if (read > 0) {
            memcpy(mail_buffer, buffer.to_str().c_str(), read + 1);
            if (tools.save_mail((*it_mail)) < 0) {
              std::cout << " error saving mail : " << oid << " to " << tools.get_mailbox_path() << std::endl;
            }
          }
          delete[] mail_buffer;
        }
      }
    }
  }

  for (auto &it : mailbox) {
    delete it.second;
  }
}

static void release_exit(std::vector<librmb::RadosMailObject *> *mail_objects, librmb::RadosCluster *cluster,
                         bool show_usage) {
  for (auto mo : *mail_objects) {
    delete mo;
  }
  cluster->deinit();
  if (show_usage == true) {
    usage_exit();
  }
}
static bool check_connection_args(std::map<std::string, std::string> &opts) {
  if (opts.find("pool") == opts.end()) {
    return false;
  }

  return true;
}
static bool sort_uid(librmb::RadosMailObject *i, librmb::RadosMailObject *j) {
  std::string::size_type sz;  // alias of size_t
  std::string t = i->get_metadata(librmb::RBOX_METADATA_MAIL_UID);
  long i_uid = std::stol(t, &sz);
  long j_uid = std::stol(j->get_metadata(librmb::RBOX_METADATA_MAIL_UID), &sz);
  return i_uid < j_uid;
}

static bool sort_recv_date(librmb::RadosMailObject *i, librmb::RadosMailObject *j) {
  std::string::size_type sz;  // alias of size_t
  std::string t = i->get_metadata(librmb::RBOX_METADATA_RECEIVED_TIME);
  long i_uid = std::stol(t, &sz);
  long j_uid = std::stol(j->get_metadata(librmb::RBOX_METADATA_RECEIVED_TIME), &sz);
  return i_uid < j_uid;
}

static bool sort_phy_size(librmb::RadosMailObject *i, librmb::RadosMailObject *j) {
  std::string::size_type sz;  // alias of size_t
  std::string t = i->get_metadata(librmb::RBOX_METADATA_PHYSICAL_SIZE);
  long i_uid = std::stol(t, &sz);
  long j_uid = std::stol(j->get_metadata(librmb::RBOX_METADATA_PHYSICAL_SIZE), &sz);
  return i_uid < j_uid;
}

static bool sort_save_date(librmb::RadosMailObject *i, librmb::RadosMailObject *j) {
  return *i->get_rados_save_date() < *j->get_rados_save_date();
}

static void load_objects(librmb::RadosStorageImpl &storage, std::vector<librmb::RadosMailObject *> &mail_objects,
                         std::string &sort_string) {
  // get load all objects metadata into memory
  librados::NObjectIterator iter(storage.get_io_ctx().nobjects_begin());
  while (iter != storage.get_io_ctx().nobjects_end()) {
    librmb::RadosMailObject *mail = new librmb::RadosMailObject();
    mail->set_oid(iter->get_oid());
    storage.load_metadata(mail);
    uint64_t object_size = 0;
    time_t save_date_rados = 0;
    storage.stat_mail(iter->get_oid(), &object_size, &save_date_rados);

    mail->set_mail_size(object_size);
    mail->set_rados_save_date(save_date_rados);
    ++iter;
    mail_objects.push_back(mail);
  }

  if (sort_string.compare("uid") == 0) {
    std::sort(mail_objects.begin(), mail_objects.end(), sort_uid);
  } else if (sort_string.compare("recv_date") == 0) {
    std::sort(mail_objects.begin(), mail_objects.end(), sort_recv_date);
  } else if (sort_string.compare("phy_size") == 0) {
    std::sort(mail_objects.begin(), mail_objects.end(), sort_phy_size);
  } else {
    std::sort(mail_objects.begin(), mail_objects.end(), sort_save_date);
  }
}

int main(int argc, const char **argv) {
  std::vector<librmb::RadosMailObject *> mail_objects;
  std::vector<const char *> args;

  std::string val;
  std::map<std::string, std::string> opts;
  std::map<std::string, std::string> xattr;
  std::string sort_type;
  unsigned int idx = 0;
  std::vector<const char *>::iterator i;
  bool is_config = false;
  bool create_config = false;
  bool update_confirmed = false;
  bool show_usage = false;
  argv_to_vec(argc, argv, &args);

  for (i = args.begin(); i != args.end();) {
    if (ceph_argparse_double_dash(&args, &i)) {
      break;
    } else if (ceph_argparse_witharg(&args, &i, &val, "-p", "--pool", static_cast<char>(NULL))) {
      opts["pool"] = val;
    } else if (ceph_argparse_witharg(&args, &i, &val, "-N", "--namespace", static_cast<char>(NULL))) {
      opts["namespace"] = val;
    } else if (ceph_argparse_witharg(&args, &i, &val, "ls", "--ls", static_cast<char>(NULL))) {
      opts["ls"] = val;
    } else if (ceph_argparse_witharg(&args, &i, &val, "get", "--get", static_cast<char>(NULL))) {
      opts["get"] = val;
    } else if (ceph_argparse_witharg(&args, &i, &val, "-O", "--out", static_cast<char>(NULL))) {
      opts["out"] = val;
    } else if (ceph_argparse_witharg(&args, &i, &val, "set", "--set", static_cast<char>(NULL))) {
      opts["set"] = val;
    } else if (ceph_argparse_witharg(&args, &i, &val, "sort", "--sort", static_cast<char>(NULL))) {
      opts["sort"] = val;
    } else if (ceph_argparse_flag(args, i, "-cfg", "--config", (char *)NULL)) {
      is_config = true;
    } else if (ceph_argparse_witharg(&args, &i, &val, "-obj", "--object", static_cast<char>(NULL))) {
      opts["cfg_obj"] = val;
    } else if (ceph_argparse_witharg(&args, &i, &val, "-U", "--update", static_cast<char>(NULL))) {
      opts["update"] = val;
    } else if (ceph_argparse_flag(args, i, "-C", "--create", (char *)NULL)) {
      create_config = true;
    } else if (ceph_argparse_flag(args, i, "-help", "--help", (char *)NULL)) {
      show_usage = true;
    } else if (ceph_argparse_flag(args, i, "-yes-i-really-really-mean-it", "--yes-i-really-really-mean-it",
                                  (char *)NULL)) {
      update_confirmed = true;

    } else {
      if (idx + 1 < args.size()) {
        xattr[args[idx]] = args[idx + 1];
        idx++;
      }
      ++idx;
      ++i;
    }
  }

  if (show_usage) {
    usage_exit();
  }

  if (args.size() <= 0 && opts.size() <= 0 && !is_config) {
    usage_exit();
  }

  librmb::RadosClusterImpl cluster;
  librmb::RadosStorageImpl storage(&cluster);

  if (strcmp(args[0], "lspools") == 0) {
    cluster.init();
    if (cluster.connect() < 0) {
      std::cout << " error opening rados connection" << std::endl;
      return 0;
    }
    std::list<std::string> vec;
    int ret = cluster.get_cluster().pool_list(vec);
    if (ret == 0) {
      for (std::list<std::string>::iterator it = vec.begin(); it != vec.end(); ++it) {
        std::cout << ' ' << *it << std::endl;
      }
    }
    cluster.deinit();
    return 0;
  }

  if (opts.find("pool") == opts.end()) {
    opts["pool"] = "mail_storage";
  }

  if (!check_connection_args(opts)) {
    usage_exit();
  }
  std::string pool_name(opts["pool"]);

  int open_connection = storage.open_connection(pool_name);
  if (open_connection < 0) {
    std::cout << " error opening rados connection" << std::endl;
    return -1;
  }

  librmb::RadosCephConfig ceph_cfg(&storage);
  std::string obj_ = opts.find("cfg_obj") != opts.end() ? opts["cfg_obj"] : ceph_cfg.get_cfg_object_name();
  ceph_cfg.set_cfg_object_name(obj_);
  ceph_cfg.set_config_valid(true);

  if (is_config) {
    //   std::cout << "found cfg: " << opts["cfg_obj"] << " " << opts["update"] << " " << opts["ls"] << "\n";
    bool has_update = opts.find("update") != opts.end();
    bool has_ls = opts.find("ls") != opts.end();
    if (has_update && has_ls) {
      usage_exit();
    }


    if (ceph_cfg.load_cfg() < 0) {
      int ret = 0;
      if (create_config) {
        ceph_cfg.save_cfg();
        std::cout << "config has been created" << std::endl;

      } else {
        std::cout << "loading config object failed " << std::endl;
        ret = -1;
      }
      cluster.deinit();
      return ret;
    }
    if (create_config) {
      std::cout << "Error: there already exists a configuration " << obj_ << std::endl;
      cluster.deinit();
      return -1;
    }

    if (has_ls) {
      std::cout << ceph_cfg.get_config()->to_string() << std::endl;
    } else if (has_update) {
      int key_val_separator_idx = opts["update"].find("=");

      if (key_val_separator_idx != std::string::npos) {
        std::string key = opts["update"].substr(0, key_val_separator_idx);
        std::string key_val = opts["update"].substr(key_val_separator_idx + 1, opts["update"].length() - 1);

        bool failed = false;

        if (!update_confirmed) {
          std::cout << "WARNING:" << std::endl;
          std::cout
              << "Changing this setting, after e-mails have been stored, could lead to a situation in which users "
                 "can no longer access their e-mail!!!"
              << std::endl;
          std::cout << "To confirm pass --yes-i-really-really-mean-it " << std::endl;
        } else {
          if (ceph_cfg.get_config()->get_key_generated_namespace().compare(key) == 0) {
            ceph_cfg.get_config()->set_generated_namespace(key_val);
          } else if (ceph_cfg.get_config()->get_key_ns_cfg().compare(key) == 0) {
            ceph_cfg.set_ns_cfg(key_val);
          } else if (ceph_cfg.get_config()->get_key_ns_suffix().compare(key) == 0) {
            ceph_cfg.set_ns_suffix(key_val);
          } else {
            std::cout << "ERROR: not a valid key: " << key << std::endl;
            std::cout << ceph_cfg.get_config()->to_string() << std::endl;
            failed = true;
          }
          if (!failed) {
            ceph_cfg.save_cfg();
          }
        }
      }
    }
    cluster.deinit();
    return 0;
  }
  librmb::RadosConfig dovecot_cfg;
  dovecot_cfg.set_config_valid(true);
  librmb::RadosDovecotCephCfgImpl cfg(&dovecot_cfg, &ceph_cfg);
  librmb::RadosNamespaceManager mgr(&storage, &cfg);
  if (opts.find("namespace") == opts.end()) {
    cluster.deinit();
    usage_exit();
  }
  std::string uid(opts["namespace"]);
  std::string ns;
  if (mgr.lookup_key(uid, &ns)) {
    std::cout << " generated ns _ " << ns << std::endl;
    storage.set_namespace(ns);
  } else {
    // use
    if (!mgr.lookup_key(uid, &ns)) {
      std::cout << " error unable to determine namespace" << std::endl;
      return -1;
    }
    std::cout << " uid : " << ns << std::endl;
    storage.set_namespace(ns);
  }

  sort_type = (opts.find("sort") != opts.end()) ? opts["sort"] : "uid";

  if (opts.find("ls") != opts.end()) {
    librmb::CmdLineParser parser(opts["ls"]);
    if (opts["ls"].compare("all") == 0 || opts["ls"].compare("-") == 0 || parser.parse_ls_string()) {
      load_objects(storage, mail_objects, sort_type);
      query_mail_storage(&mail_objects, &parser, false, nullptr);
    } else {
      // tear down.
      release_exit(&mail_objects, &cluster, true);
    }
  } else if (opts.find("get") != opts.end()) {
    librmb::CmdLineParser parser(opts["get"]);

    if (opts.find("out") != opts.end()) {
      parser.set_output_dir(opts["out"]);
    } else {
      char outpath[PATH_MAX];
      char *home = getenv("HOME");
      if (home != NULL) {
        snprintf(outpath, sizeof(outpath), "%s/rmb", home);
      } else {
        snprintf(outpath, sizeof(outpath), "rmb");
      }
      parser.set_output_dir(outpath);
    }

    if (opts["get"].compare("all") == 0 || opts["get"].compare("-") == 0 || parser.parse_ls_string()) {
      // get load all objects metadata into memory
      load_objects(storage, mail_objects, sort_type);
      query_mail_storage(&mail_objects, &parser, true, &storage);
    } else {
      // tear down.
      release_exit(&mail_objects, &cluster, true);
    }
  } else if (opts.find("set") != opts.end()) {
    std::string oid = opts["set"];
    if (oid.empty() || xattr.size() < 1) {
      release_exit(&mail_objects, &cluster, true);
    }

    for (std::map<std::string, std::string>::iterator it = xattr.begin(); it != xattr.end(); ++it) {
      std::cout << oid << "=> " << it->first << " = " << it->second << '\n';
      librmb::rbox_metadata_key ke = static_cast<librmb::rbox_metadata_key>(it->first[0]);
      std::string value = it->second;
      if (librmb::RadosUtils::is_date_attribute(ke)) {
        if (!librmb::RadosUtils::is_numeric(value)) {
          value = librmb::RadosUtils::convert_string_to_date(value);
        }
      }
      librmb::RadosMetadata attr(ke, value);
      storage.set_metadata(oid, attr);
    }
  }
  // tear down.
  release_exit(&mail_objects, &cluster, false);
}

