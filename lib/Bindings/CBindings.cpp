/*
 SPDX-License-Identifier: LGPL-2.1-or-later */
/* SPDX-FileCopyrightText: SUSE LLC */

#include "libtukit.h"
#include "Log.hpp"
#include "Reboot.hpp"
#include "Transaction.hpp"
#include "SnapshotManager.hpp"
#include <algorithm>
#include <exception>
#include <thread>
#include <string.h>
#include <vector>

// TODO: clean includes
#define STACK_SIZE (1024 * 1024 * 8) /* Stack size for cloned child */
#include <err.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/limits.h>

using namespace TransactionalUpdate;
thread_local std::string errmsg;

const char* tukit_get_errmsg() {
    return errmsg.c_str();
}
void tukit_set_loglevel(loglevel lv) {
    tulog.level = static_cast<TULogLevel>(lv);
}
tukit_tx tukit_new_tx() {
    Transaction* transaction = nullptr;
    try {
        transaction = new Transaction;
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
    }
    return reinterpret_cast<tukit_tx>(transaction);
}
void tukit_free_tx(tukit_tx tx) {
    if (tx != nullptr) {
        delete reinterpret_cast<Transaction*>(tx);
    }
}
int tukit_tx_init(tukit_tx tx, char* base) {
    Transaction* transaction = reinterpret_cast<Transaction*>(tx);
    try {
        if (std::string(base).empty())
            transaction->init("active");
        else
            transaction->init(base);
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return -1;
    }
    return 0;
}
int tukit_tx_init_with_desc(tukit_tx tx, char* base, char* description) {
    Transaction* transaction = reinterpret_cast<Transaction*>(tx);
    if (std::string(base).empty())
        base=(char*)"active";
    try {
        if (description == nullptr)
            transaction->init(base);
        else
            transaction->init(base, description);
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return -1;
    }
    return 0;
}
int tukit_tx_discard_if_unchanged(tukit_tx tx, int discard) {
    Transaction* transaction = reinterpret_cast<Transaction*>(tx);
    try {
        transaction->setDiscardIfUnchanged(discard);
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return -1;
    }
    return 0;
}
int tukit_tx_resume(tukit_tx tx, char* id) {
    Transaction* transaction = reinterpret_cast<Transaction*>(tx);
    try {
        transaction->resume(id);
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return -1;
    }
    return 0;
}
int tukit_tx_execute(tukit_tx tx, char* argv[], const char* output[]) {
    Transaction* transaction = reinterpret_cast<Transaction*>(tx);
    std::string buffer;
    try {
        int ret = transaction->execute(argv, &buffer);
        if (output) {
            *output = strdup(buffer.c_str());
        }
        return ret;
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return -1;
    }
}
int tukit_tx_call_ext(tukit_tx tx, char* argv[], const char* output[]) {
    Transaction* transaction = reinterpret_cast<Transaction*>(tx);
    std::string buffer;
    try {
        int ret = transaction->callExt(argv, &buffer);
        if (output) {
            *output = strdup(buffer.c_str());
        }
        return ret;
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return -1;
    }
}
static int tukit_tx_call_fn_inner(tukit_tx tx, int (*callback)(void*), void* ctx) {
    Transaction* transaction = reinterpret_cast<Transaction*>(tx);
    std::string buffer;
    auto cbfn = [&]() {
        int res = callback(ctx);
        return res;
    };

    try {
        int ret = transaction->callFn(cbfn, &buffer);
        return ret;
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return -1;
    }
}
struct tuki_thread_data {
    tukit_tx tx;
    int (*callback)(void*);
    void* ctx;
};
static int child_func(void *arg)
{
    tuki_thread_data* fb = (tuki_thread_data*)arg;
    tukit_tx_call_fn_inner(fb->tx, fb->callback, fb->ctx);
    _exit(0);
    return 0;
}
int tukit_tx_call_fn(tukit_tx tx, int (*callback)(void*), void* ctx, const char* __output[]) {
    char* stack = (char*)malloc(STACK_SIZE);
    if (!stack) {
      perror("malloc");
      exit(1);
    }

    tuki_thread_data fb = {
        .tx = tx,
        .callback = callback,
        .ctx = ctx,
    };

    tukit_set_loglevel(Debug);
    tukit_tx_init(tx, (char*)"");

    pid_t pid = clone(child_func, stack + STACK_SIZE, CLONE_VM | CLONE_IO | SIGCHLD, &fb);
    if (pid == -1) {
      perror("clone");
      exit(1);
    }

    int status;
    if (waitpid(pid, &status, 0) == -1) {
      perror("wait");
      exit(1);
    }

    tukit_tx_finalize(tx);

    return 0;
}
int tukit_tx_finalize(tukit_tx tx) {
    Transaction* transaction = reinterpret_cast<Transaction*>(tx);
    try {
        transaction->finalize();
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return -1;
    }
    return 0;
}
int tukit_tx_keep(tukit_tx tx) {
    Transaction* transaction = reinterpret_cast<Transaction*>(tx);
    try {
        transaction->keep();
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return -1;
    }
    return 0;
}
int tukit_tx_send_signal(tukit_tx tx, int signal) {
    Transaction* transaction = reinterpret_cast<Transaction*>(tx);
    try {
        transaction->sendSignal(signal);
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return -1;
    }
    return 0;
}
int tukit_tx_is_initialized(tukit_tx tx) {
    Transaction* transaction = reinterpret_cast<Transaction*>(tx);
    return transaction->isInitialized();
}
/* Free return string with free() */
const char* tukit_tx_get_snapshot(tukit_tx tx) {
    Transaction* transaction = reinterpret_cast<Transaction*>(tx);
    try {
        return strdup(transaction->getSnapshot().c_str());
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return nullptr;
    }
}
const char* tukit_tx_get_root(tukit_tx tx) {
    Transaction* transaction = reinterpret_cast<Transaction*>(tx);
    try {
        return transaction->getRoot().c_str();
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return nullptr;
    }
}

const char* tukit_sm_get_current() {
    try {
        std::unique_ptr<TransactionalUpdate::SnapshotManager> snapshotMgr = TransactionalUpdate::SnapshotFactory::get();
        return strdup(snapshotMgr->getCurrent().c_str());
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return nullptr;
    }
}

const char* tukit_sm_get_default() {
    try {
        std::unique_ptr<TransactionalUpdate::SnapshotManager> snapshotMgr = TransactionalUpdate::SnapshotFactory::get();
        return strdup(snapshotMgr->getDefault().c_str());
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return nullptr;
    }
}

tukit_sm_list tukit_sm_get_list(size_t* len, const char* columns) {
    try {
        std::unique_ptr<TransactionalUpdate::SnapshotManager> snapshotMgr = TransactionalUpdate::SnapshotFactory::get();
        auto list = new std::deque<std::map<std::string, std::string>>;
        try {
            *list = snapshotMgr->getList(columns);
            *len = list->size();
            return reinterpret_cast<tukit_sm_list>(list);
        } catch (const std::exception &e) {
            delete list;
            fprintf(stderr, "ERROR: %s\n", e.what());
            errmsg = e.what();
            return nullptr;
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return nullptr;
    }
}

const char* tukit_sm_get_list_value(tukit_sm_list list, size_t row, char* column) {
    try {
        auto result = reinterpret_cast<std::deque<std::map<std::string, std::string>>*>(list);
        return result->at(row)[column].c_str();
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return nullptr;
    }
}

void tukit_free_sm_list(tukit_sm_list list) {
    auto result = reinterpret_cast<std::deque<std::map<std::string, std::string>>*>(list);
    delete result;
}

int tukit_sm_deletesnap(const char* id) {
    try {
        std::unique_ptr<TransactionalUpdate::SnapshotManager> snapshotMgr = TransactionalUpdate::SnapshotFactory::get();
        snapshotMgr->deleteSnap(id);
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return -1;
    }
}

int tukit_sm_rollbackto(const char* id) {
    try {
        std::unique_ptr<TransactionalUpdate::SnapshotManager> snapshotMgr = TransactionalUpdate::SnapshotFactory::get();
        snapshotMgr->rollbackTo(id);
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return -1;
    }
}

int tukit_reboot(const char* method) {
    try {
        auto rebootmgr = Reboot{method};
        rebootmgr.reboot();
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        errmsg = e.what();
        return -1;
    }
    return 0;
}
