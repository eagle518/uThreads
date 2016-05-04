/*******************************************************************************
 *     Copyright © 2015, 2016 Saman Barghi
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *******************************************************************************/

#include <runtime/schedulers/Scheduler.h>
#include "IOHandler.h"
#include "Network.h"
#include "runtime/kThread.h"
#include <unistd.h>
#include <sys/types.h>
#include <iostream>
#include <sstream>

IOHandler::IOHandler(Cluster& cluster): bulkCounter(0), localCluster(&cluster), ioKT(cluster, &IOHandler::pollerFunc, (ptr_t)this){}

IOHandler* IOHandler::create(Cluster& cluster){
    IOHandler* ioh = nullptr;
#if defined (__linux__)
    ioh = new EpollIOHandler(cluster);
#else
#error unsupported system: only __linux__ supported at this moment
#endif
    return ioh;
}

void IOHandler::open(PollData &pd){
    assert(pd.fd > 0);

    std::lock_guard<std::mutex> pdlock(pd.mtx);
    int res = _Open(pd.fd, pd);
    if(res == 0)
        pd.opened.store(true, std::memory_order_relaxed);
    else{
        std::cerr << "EPOLL_ERROR: " << errno << std::endl;
        //TODO: this should be changed to an exception
        exit(EXIT_FAILURE);
    }
    //TODO: handle epoll errors
}
void IOHandler::wait(PollData& pd, int flag){
    assert(pd.fd > 0);
    if(flag & Flag::UT_IOREAD) block(pd, true);
    if(flag & Flag::UT_IOWRITE) block(pd, false);
}
void IOHandler::block(PollData &pd, bool isRead){

    if(!pd.opened.load(std::memory_order_relaxed)) open(pd);
    uThread** utp = isRead ? &pd.rut : &pd.wut;
    //TODO:check other states

    if(slowpath(*utp == POLL_READY))    //This is unlikely since we just did a nonblocking read
    {
        *utp = nullptr;  //consume the notification and return;
        return;
    }else if(*utp > POLL_WAIT)
        std::cerr << "Exception on open rut" << std::endl;

    //This does not need synchronization, since only a single thread
    //will access it before and after blocking
    pd.isBlockingOnRead = isRead;

    kThread::currentKT->currentUT->suspend((funcvoid2_t)IOHandler::postSwitchFunc, (void*)&pd);
    //ask for immediate suspension so the possible closing/notifications do not get lost
    //when epoll returns this ut will be back on readyQueue and pick up from here
}
void IOHandler::postSwitchFunc(void* ut, void* args){
    assert(args != nullptr);
    assert(ut != nullptr);

    uThread* old = (uThread*)ut;
    PollData* pd = (PollData*) args;
    if(pd->closing) return;
    uThread** utp = pd->isBlockingOnRead ? &pd->rut : &pd->wut;

    std::lock_guard<std::mutex> pdlock(pd->mtx);
    if(*utp == POLL_READY){
        *utp = nullptr;         //consume the notification and resume
        old->resume();
    }else if(*utp == nullptr){
        *utp = old;
    }else
        std::cerr << "Exception on rut"<< std::endl;
}
int IOHandler::close(PollData &pd){

    std::lock_guard<std::mutex> pdlock(pd.mtx);

    int flag = 0;
    if(pd.rut > POLL_WAIT)
        flag | Flag::UT_IOREAD;
    if(pd.wut > POLL_WAIT);
        flag | Flag::UT_IOWRITE;

    if(flag)
        unblock(pd, flag);

    pd.closing = true;
    //remove from underlying poll structure
    int res = _Close(pd.fd);

    //pd.reset();
    //TODO: handle epoll errors
    pd.reset();
    pollCache.pushPollData(&pd);
    return res;
}

void IOHandler::poll(int timeout, int flag){
    _Poll(timeout);
}

void IOHandler::reset(PollData& pd){
    std::lock_guard<std::mutex> pdlock(pd.mtx);
    pd.reset();
}

void IOHandler::unblock(PollData &pd, int flag){

    std::lock_guard<std::mutex> pdlock(pd.mtx);
    //if it's closing no need to process
    if(slowpath(pd.closing)) return;

    uThread **rut = &pd.rut, **wut = &pd.wut;
    uThread *rold = *rut, *wold = *wut;

    if(flag & Flag::UT_IOREAD){
        //if(rold == POLL_READY) //do nothing
        if(rold == nullptr || rold == POLL_WAIT)
           *rut = POLL_READY;
        else if(rold > POLL_WAIT){
            *rut = nullptr;
            rold->resume();
        }
    }
    if(flag & Flag::UT_IOWRITE){
        //if(wold == POLL_READY) do nothing
        if(wold == nullptr || wold == POLL_WAIT)
           *wut = POLL_READY;
        else if(wold > POLL_WAIT){
            *wut = nullptr;
            wold->resume();
        }
    }
}

void IOHandler::unblockBulk(PollData &pd, int flag){

    std::lock_guard<std::mutex> pdlock(pd.mtx);
    //if it's closing no need to process
    if(slowpath(pd.closing)) return;

    uThread **rut = &pd.rut, **wut = &pd.wut;
    uThread *rold = *rut, *wold = *wut;

    if(flag & Flag::UT_IOREAD){
        //if(rold == POLL_READY) //do nothing
        if(rold == nullptr || rold == POLL_WAIT)
           *rut = POLL_READY;
        else if(rold > POLL_WAIT){
            *rut = nullptr;
            rold->state = uThread::State::READY;
            Scheduler::prepareBulkPush(rold);
            bulkCounter++;
        }
    }
    if(flag & Flag::UT_IOWRITE){
        //if(wold == POLL_READY) do nothing
        if(wold == nullptr || wold == POLL_WAIT)
           *wut = POLL_READY;
        else if(wold > POLL_WAIT){
            *wut = nullptr;
            wold->state = uThread::State::READY;
            Scheduler::prepareBulkPush(wold);
            bulkCounter++;
        }
    }
    /* It is the responsibility of the caller function
     * to call scheduleMany to schedule the piled-up
     * uThreads on the related cluster.
     */
}

void IOHandler::PollReady(PollData &pd, int flag){
    unblock(pd, flag);
}
void IOHandler::PollReadyBulk(PollData &pd, int flag, bool isLast){
    unblockBulk(pd, flag);
    //if this is the last item return by the poller
    //Bulk push everything to the related cluster ready Queue
    if(slowpath(isLast) && bulkCounter >0){
        localCluster->scheduler->schedule(bulkQueue, bulkCounter);
        bulkCounter =0;
    }
}

void IOHandler::pollerFunc(void* ioh){
    IOHandler* cioh = (IOHandler*)ioh;
    while(true){
       cioh->poll(-1, 0);
   }
}
