#pragma once

#include <AsyncServerSocket.cc>
#include <asynclib>
#include <errno.h>
#include <fcntl.h>
#include <functional>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <unistd.h>

class EventLoop {
  using EventHandler = std::function<void(
      char *buffer, uint len, std::function<void(char *buffer, uint len)>)>;
  using FileDescriptor = int;

public:
  enum Event {
    WRITE,
    READ,
    ERROR,
  };
  EventLoop(uint cnt = 1024) : maxEventsCount(cnt){};
  ~EventLoop() {
    if (this->epollFd > 0)
      close(this->epollFd);
    if (this->events != nullptr)
      delete[] this->events;
  }
  bool init() {
    this->epollFd = epoll_create1(0);
    this->events = new epoll_event[this->maxEventsCount];
    if (this->epollFd < 0 || this->events == nullptr) {
      perror("epoll_create1");
      exit(EXIT_FAILURE);
    }
  }

  bool regist(AsyncServerSocket &socket, EventLoop::Event eventType,
              EventHandler callback) {
    // epoll does not have an api to query for existence of a file descriptor
    // we have to use a map to store the file descriptor and its event type
    if (has(fdEventMap, socket.getSocketFd())) {
      if (has(fdEventMap[socket.getSocketFd()], eventType)) {
        fdEventMap[socket.getSocketFd()][eventType].emplace_back(callback);
      } else {
        fdEventMap[socket.getSocketFd()][eventType] = {callback};
      }
    } else {
      // we assert that the existence of the file descriptor in map is
      // consistent with its existence in epoll
      epoll_event event = {
          .events = EPOLLIN | EPOLLOUT | EPOLLET,
          .data = {.fd = socket.getSocketFd()},
      };
      if (epoll_ctl(this->epollFd, EPOLL_CTL_ADD, socket.getSocketFd(),
                    &event) < 0) {
        return false;
      }
      fdEventMap[socket.getSocketFd()] = {{eventType, {callback}}};
    }
    return true;
  }

  bool registClient(FileDescriptor &socket) {
    // the reason why we not store the file descriptor in map is that we can
    // find the relation in this->fdClientToServerMap, in fact the events of
    // server socket triggered by client socket
    epoll_event event = {
        .events = EPOLLIN | EPOLLOUT | EPOLLET,
        .data = {.fd = socket},
    };
    if (epoll_ctl(this->epollFd, EPOLL_CTL_ADD, socket, &event) < 0) {
      return false;
    }
    return true;
  }

  bool unregist(AsyncServerSocket &socket, EventLoop::Event eventType) {
    if (has(fdEventMap, socket.getSocketFd())) {
      if (has(fdEventMap[socket.getSocketFd()], eventType)) {
        fdEventMap[socket.getSocketFd()].erase(eventType);
      }
      if (fdEventMap[socket.getSocketFd()].empty()) {
        fdEventMap.erase(socket.getSocketFd());
        epoll_event event = {
            .events = EPOLLIN | EPOLLOUT | EPOLLET,
            .data = {.fd = socket.getSocketFd()},
        };
        // according to the above assertion, it should be removed from epoll
        // here
        return epoll_ctl(this->epollFd, EPOLL_CTL_DEL, socket.getSocketFd(),
                         &event) >= 0;
      }
    }
  }

  void run() {
    while (true) {
      /* TODO: Timer Heap phase */
      /* TODO: Event Queue phase */
      /* I/O phase */
      int howManyTriggered = epoll_wait(
          this->epollFd, this->events, this->maxEventsCount,
          EventLoop::WAIT_FOREVER /* blocking forever if no event comming */
      );

      for (uint16_t i = 0; i < howManyTriggered; ++i) {
        FileDescriptor fdTriggered = this->events[i].data.fd;

        /* try accept connection on the file descriptor triggered */
        if (has(fdEventMap, fdTriggered)) {
          while (1) {
            sockaddr clientAddr;
            socklen_t clientAddrLen = sizeof(clientAddr);
            FileDescriptor clientSocketFd = -1;
            char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

            clientSocketFd = accept(fdTriggered, &clientAddr, &clientAddrLen);
            if (clientSocketFd == -1) {
              if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                /* We have processed all incoming
                   connections. */
                break;
              } else {
                perror("accept");
                break;
              }
            }

            /* Make the incoming socket non-blocking and add it to the epoll */
            if (AsyncServerSocket::makeNonBlocking(clientSocketFd) == -1) {
              close(clientSocketFd);
              continue;
            }

            if (this->registClient(clientSocketFd)) {
              // we store the relation for emitting event on server socket
              fdClientToServerMap[clientSocketFd] = fdTriggered;
            } else {
              close(clientSocketFd);
              continue;
            }
          }
          continue;
        }

        int eventsTriggered = this->events[i].events;
        FileDescriptor fdServer = fdTriggered;
        if (has(fdClientToServerMap, fdTriggered)) {
          fdServer = fdClientToServerMap[fdTriggered];
        }

        if (eventsTriggered & EPOLLERR || eventsTriggered & EPOLLHUP) {
          if (has(fdEventMap, fdServer) &&
              has(fdEventMap[fdServer], Event::ERROR)) {
            for (auto callback : fdEventMap[fdServer][Event::ERROR]) {
              callback(nullptr, 0, nullptr);
            }
          }
        }
        if (eventsTriggered & EPOLLIN) {
          if (has(fdEventMap, fdServer) &&
              has(fdEventMap[fdServer], Event::READ)) {
            this->readAndCall(fdTriggered /* client socket */);
          }
        }
        if (eventsTriggered & EPOLLOUT) {
          if (has(fdEventMap, fdServer) &&
              has(fdEventMap[fdServer], Event::WRITE)) {
            for (auto callback : fdEventMap[fdServer][Event::WRITE]) {
              callback(nullptr, 0,
                       this->closureWriteTo(fdTriggered /* client socket */));
            }
          }
        }
      }
    }
  }

private:
  int epollFd = -1;
  uint maxEventsCount;
  epoll_event *events;
  // client socket -> server socket
  std::unordered_map<FileDescriptor, FileDescriptor> fdClientToServerMap;
  // server socket -> handlers map
  std::unordered_map<
      FileDescriptor,
      std::unordered_map<EventLoop::Event, std::vector<EventHandler>>>
      fdEventMap;

  static const int WAIT_FOREVER = -1;

  template <typename K, typename V>
  bool has(std::unordered_map<K, V> map, K key) {
    return map.find(key) != map.end();
  }

  void readAndCall(FileDescriptor fd) {
    uint32_t readed = 0;
    uint32_t currentLength = 1024;
    bool closedByPeer = false;
    char *buf = new char[currentLength];
    while (true) {
      int count = SocketLib::read(fd, buf + readed, currentLength - readed);
      if (count == -1) {
        /* we have read all data */
        if (errno != EAGAIN) {
          /* TODO: remote has closed the connection. */
          closedByPeer = true;
        }
        break;
      } else if (count == 0) {
        /* TODO: remote has closed the connection. */
        closedByPeer = true;
        break;
      }
      readed += count;
      if (readed == currentLength) {
        currentLength *= 2;
        char *newBuf = new char[currentLength];
        memcpy(newBuf, buf, readed);
        delete[] buf;
        buf = newBuf;
      }
    }
    if (readed > 0) {
      for (auto callback : fdEventMap[fdClientToServerMap[fd]][Event::READ]) {
        callback(buf, readed, nullptr);
      }
    }
    if (closedByPeer) {
      close(fd);
    }
    delete[] buf;
  }

  std::function<void(char *buffer, uint len)>
  closureWriteTo(FileDescriptor fd) {
    return [fd](char *buffer, uint len) {
      uint written = 0;
      while (written < len) {
        int count = SocketLib::write(fd, buffer + written, len - written);
        if (count == -1) {
          if (errno == EAGAIN) {
            /* we have written all data */
            break;
          }
        } else if (count == 0) {
          /* remote has closed the connection. */
          break;
        }
        written += count;
      }
      close(fd);
    };
  }
};
