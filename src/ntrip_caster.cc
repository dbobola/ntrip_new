#include "ntrip/ntrip_caster.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <algorithm>
#include <string>
#include <vector>
#include <memory>
#include <limits>

#include "ntrip/ntrip_util.h"
#include "cmake_definition.h.in"


namespace libntrip {

namespace {

constexpr int kBufferSize = 65536;

inline
int EpollRegister(int epoll_fd, int fd) {
  struct epoll_event ev;
  int ret, flags;
  // Important: make the fd non-blocking.
  flags = fcntl(fd, F_GETFL);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  ev.events = EPOLLIN;
  // ev.events = EPOLLIN | EPOLLET;
  ev.data.fd = fd;
  do {
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
  } while ((ret < 0) && (errno == EINTR));
  return ret;
}

inline
int EpollUnregister(int epoll_fd, int fd) {
  int ret;
  do {
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
  } while ((ret < 0) && (errno == EINTR));
  return ret;
}

inline
void ClearCilentConnection(int epoll_fd, std::list<int> *list) {
  if ((list != nullptr) && (!list->empty())) {
    auto it = list->begin();
    while (it != list->end()) {
      EpollUnregister(epoll_fd, *it);
      close(*it);
      it = list->erase(it);
    }
  }
}

inline
void ClearAllConnection(int epoll_fd, std::list<MountPointInformation> *list) {
  if ((list != nullptr) && (!list->empty())) {
    auto it = list->begin();
    while (it != list->end()) {
      ClearCilentConnection(epoll_fd, &(it->client_socket_list));
      EpollUnregister(epoll_fd, it->server_fd);
      close(it->server_fd);
      it = list->erase(it);
    }
  }
}

inline
int StringSplit(std::string const& src, std::string const& div_str,
    std::vector<std::string>* out, bool append_div_str = false) {
  if (out == nullptr) return -1;
  out->clear();
  std::string::size_type pos_begin = 0;
  std::string::size_type div_pos_begin = src.find(div_str);
  std::size_t src_len = src.size();
  std::size_t div_str_len = div_str.size();
  std::size_t len;
  std::string item;
  while (div_pos_begin != std::string::npos) {
    len = div_pos_begin-pos_begin;
    if (len > 0) {
      item = src.substr(pos_begin, len);
      if (append_div_str) item += div_str;
      out->push_back(item);
    }
    pos_begin = div_pos_begin + div_str_len;
    div_pos_begin = src.find(div_str, pos_begin);
  }
  if (pos_begin+div_str_len <= src_len) {
    item = src.substr(pos_begin, src_len-pos_begin);
      if (append_div_str) item += div_str;
      out->push_back(item);
  }
  return 0;
}

}  // namespace


//
// Public.
//

NtripCaster::~NtripCaster() {
  Stop();
}

bool NtripCaster::Run(void) {
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(struct sockaddr_in));
  std::cout << "[DEBUG] Run method entered" << std::endl;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(server_port_);
  if (server_ip_.empty()) {
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else {
    server_addr.sin_addr.s_addr = inet_addr(server_ip_.c_str());
  }
  std::cout << "[DEBUG] Creating socket..." << std::endl;
  listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_sock_ == -1) {
    std::cout << "[ERROR] Socket creation failed: " << strerror(errno) << std::endl;
    exit(1);
  }
  std::cout << "[DEBUG] Binding socket to port " << server_port_ << "..." << std::endl;
  if (bind(listen_sock_, reinterpret_cast<struct sockaddr*>(&server_addr),
      sizeof(struct sockaddr)) == -1) {
    std::cout << "[ERROR] Bind failed: " << strerror(errno) << std::endl;
    exit(1);
  }
  std::cout << "[DEBUG] Starting listen..." << std::endl;
  if (listen(listen_sock_, 5) == -1) {
    std::cout << "[ERROR] Listen failed: " << strerror(errno) << std::endl;
    exit(1);
  }
  std::cout << "[DEBUG] Creating epoll events array..." << std::endl;
  epoll_events_ = new struct epoll_event[max_count_];
  if (epoll_events_ == nullptr) {
    std::cout << "[ERROR] Failed to allocate epoll events" << std::endl;
    exit(1);
  }
  std::cout << "[DEBUG] Creating epoll instance..." << std::endl;
  epoll_fd_ = epoll_create(max_count_);
  if (epoll_fd_ == -1) {
    std::cout << "[ERROR] Epoll creation failed: " << strerror(errno) << std::endl;
    exit(1);
  }
  std::cout << "[DEBUG] Registering listen socket with epoll..." << std::endl;
  EpollRegister(epoll_fd_, listen_sock_);
  std::cout << "[DEBUG] Setting service as running..." << std::endl;
  service_is_running_.store(true);
  std::cout << "[DEBUG] Starting thread..." << std::endl;
  thread_.reset(&NtripCaster::ThreadHandler, this);
  std::cout << "[DEBUG] Run method completed successfully" << std::endl;
  return true;
}

void NtripCaster::Stop(void) {
  service_is_running_.store(false);
  ClearAllConnection(epoll_fd_, &mount_point_infos_);
  if (listen_sock_ > 0) {
    EpollUnregister(epoll_fd_, listen_sock_);
    close(listen_sock_);
  }
  if (epoll_fd_ > 0) {
    close(epoll_fd_);
  }
  if (epoll_events_ != nullptr) {
    delete [] epoll_events_;
    epoll_events_ = nullptr;
  }
  ntrip_str_list_.clear();
  thread_.join();
}

//
// Private.
//

void NtripCaster::ThreadHandler(void) {
  std::cout << "[DEBUG] ThreadHandler method entered" << std::endl;
  int ret;
  int alive_count;
  
  std::unique_ptr<char[]> buffer(
      new char[kBufferSize], std::default_delete<char[]>());
  printf("NtripCaster service running...\n");
  std::cout << "[DEBUG] Entering main epoll loop..." << std::endl;
  while (1) {
    ret = epoll_wait(epoll_fd_, epoll_events_, max_count_, time_out_);
    if (ret == 0) {
      // printf("Epoll timeout\n");
      continue;
    } else if (ret == -1) {
      printf("Epoll error\n");
      break;
    } else {
      alive_count = ret;
      for (int i = 0; i < alive_count; ++i) {
        if (epoll_events_[i].data.fd == listen_sock_) {
          // If the server listens to the EPOLLIN events,
          // accept new client and add this socket to epoll listen list.
          if (epoll_events_[i].events & EPOLLIN) {
            AcceptNewConnect();
          }
        } else {
          if (epoll_events_[i].events & EPOLLIN) {
            int ret = recv(epoll_events_[i].data.fd,
                buffer.get(), kBufferSize, 0);
            if (ret > 0) {
              // Start parsing received's remote data.
              if (ParseData(epoll_events_[i].data.fd, buffer.get(), ret) < 0) {
                close(epoll_events_[i].data.fd);
              }
            } else {
              Disconnect(epoll_events_[i].data.fd);
            }
          }
        }
      }
    }
  }
  printf("NtripCaster service done.\n");
  service_is_running_.store(false);
}

int NtripCaster::AcceptNewConnect(void) {
  struct sockaddr_in client_addr;
  memset(&client_addr, 0, sizeof(struct sockaddr_in));
  socklen_t clilen = sizeof(struct sockaddr);
  int new_sock = accept(listen_sock_, (struct sockaddr*)&client_addr, &clilen);
  // TCP socket keepalive.
  int keepalive = 1;     // Enable keepalive attributes.
  int keepidle = 30;     // Time out for starting detection.
  int keepinterval = 5;  // Time interval for sending packets during detection.
  int keepcount = 3;     // Max times for sending packets during detection.
  setsockopt(new_sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive,
             sizeof(keepalive));
  setsockopt(new_sock, SOL_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
  setsockopt(new_sock, SOL_TCP, TCP_KEEPINTVL, &keepinterval,
             sizeof(keepinterval));
  setsockopt(new_sock, SOL_TCP, TCP_KEEPCNT, &keepcount, sizeof(keepcount));
  EpollRegister(epoll_fd_, new_sock);
  return new_sock;
}

void NtripCaster::Disconnect(int socket_fd) {
  auto it = mount_point_infos_.begin();
  while (it != mount_point_infos_.end()) {
    if (it->server_fd == socket_fd) {  // It is ntrip server.
      printf("NtripServer disconnect.\n");
      ClearCilentConnection(epoll_fd_, &(it->client_socket_list));
      // Remove mount point information from source table list.
      std::string ntrip_str = "STR;" + it->mountpoint + ";";
      auto str_it = ntrip_str_list_.begin();
      while (str_it != ntrip_str_list_.end()) {
        if (str_it->find(ntrip_str) != std::string::npos) {
          ntrip_str_list_.erase(str_it);
          break;
        }
        ++str_it;
      }
      mount_point_infos_.erase(it);
      break;
    } else {  // is ntrip client.
      bool find_client = false;
      auto cli_it = it->client_socket_list.begin();
      while (cli_it != it->client_socket_list.end()) {
        if (*cli_it == socket_fd) {
          printf("NtripClient disconnect\n");
          it->client_socket_list.erase(cli_it);
          find_client = true;
          break;
        }
        ++cli_it;
      }
      if (find_client == true) {
        break;
      }
    }
    ++it;
  }
  close(socket_fd);
}

int NtripCaster::ParseData(
    int socket_fd, char const* buffer, int buffer_len) {
  int retval = -1;
  std::vector<std::string> request_lines;
  std::string str(buffer, buffer+buffer_len);
  if ((str.find("GET /") != std::string::npos) ||
      (str.find("POST /") != std::string::npos)) {
    // printf("%s\n", str.c_str());
    StringSplit(str, "\r\n", &request_lines, true);
    // Server request to connect to Caster.
    if ((str.find("POST /") != std::string::npos) &&
        (str.find("HTTP/1.1") != std::string::npos)) {
      retval = ServerConnectRequest(request_lines, socket_fd);
    } else if ((str.find("GET /") != std::string::npos) &&
        (str.find("HTTP/1.1") != std::string::npos ||
        str.find("HTTP/1.0") != std::string::npos)) {
      // retval = DealClientConnectRequest(&request_lines, sock);
      retval = ClientConnectRequest(request_lines, socket_fd);
    }
  } else {
    // Data sent by Server, it needs to be forwarded to connected client.
    if ((retval = TryToForwardServerData(socket_fd, buffer, buffer_len)) < 0) {
      // If Caster as a base station, it maybe needs to deal the GGA data
      // sent by the ntrip client, now it's just printing.
      if (str.find("$GPGGA,") != std::string::npos ||
          str.find("$GNGGA,") != std::string::npos) {
        if (!BccCheckSumCompareForGGA(str.c_str())) {
          // printf("Check sum pass\n");
          // printf("%s", str.c_str());
          
          // Parse GGA position for potential auto-selection updates
          double client_lat, client_lon;
          if (ParsePositionFromGGA(str, &client_lat, &client_lon) == 0) {
            printf("Client GGA position: lat=%.6f, lon=%.6f\n", client_lat, client_lon);
            
            // Check if this client is connected to an "auto" mountpoint
            // and potentially suggest a better base station
            // (This could be extended to support dynamic reconnection)
          }
        }
        retval = 0;
      }
    }
  }
  request_lines.clear();
  return retval;
}

void NtripCaster::SendSourceTableData(int socket_fd) {
  std::string ntrip_str = "";
  for (auto const& str : ntrip_str_list_) {
    ntrip_str += str;
  }
  std::unique_ptr<char[]> datetime(
      new char[128], std::default_delete<char[]>());
  time_t now;
  time(&now);
  struct tm *tm_now = localtime(&now);
  strftime(datetime.get(), 128, "%x %H:%M:%S %Z", tm_now);
  std::unique_ptr<char[]> buffer(
      new char[kBufferSize], std::default_delete<char[]>());
  int len = snprintf(buffer.get(), kBufferSize-1,
      "SOURCETABLE 200 OK\r\n"
      "Server: %s\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: %d\r\n"
      "Date: %s\r\n"
      "\r\n"
      "%s"
      "ENDSOURCETABLE\r\n",
      kCasterAgent, static_cast<int>(ntrip_str.size()),
      datetime.get(), ntrip_str.c_str());
  if (send(socket_fd, buffer.get(), len, 0) != len) {
    printf("Send source table failed!!!\n");
  }
}

// TODO(mengyuming@hotmail.com) : Multiple connections still have problems.
int NtripCaster::TryToForwardServerData(
    int socket_fd, char const* buffer, int buffer_len) {
  for (auto& info : mount_point_infos_) {
    if (info.server_fd == socket_fd) {
      for (auto& fd : info.client_socket_list) {
        if (send(fd, buffer, buffer_len, 0) != buffer_len) ;
      }
      return 0;
    }
  }
  return -1;
}

int NtripCaster::ServerConnectRequest(
    std::vector<std::string> const& lines, int socket_fd) {
  std::string mount_point;
  std::string user_passwd_base64;
  std::string user_passwd;
  std::string user;
  std::string passwd;
  std::string ntrip_str;
  double latitude = 0.0;
  double longitude = 0.0;
  bool has_position = false;
  
  for (auto const& line : lines) {
    if (line.find("POST") != std::string::npos) {
      auto pos_beg = line.find('/');
      auto pos_end = line.find(' ', pos_beg);
      if (pos_beg == std::string::npos || pos_end == std::string::npos ||
          pos_beg >= pos_end) {
        return -1;
      }
      mount_point = line.substr(pos_beg+1, pos_end-pos_beg-1);
      // Check mountpoint.
      for (auto const& info : mount_point_infos_) {
        if (mount_point == info.mountpoint) {
          printf("MountPoint already used!!!\n");
          if (send(socket_fd, "ERROR - Bad Password\r\n", 22, 0) != 22) ;
          return -1;
        }
      }
    } else if (line.find("Authorization: Basic") != std::string::npos) {
      auto pos_beg = line.find_last_of(' ');
      auto pos_end = line.find('\r', pos_beg);
      if (pos_beg == std::string::npos || pos_end == std::string::npos ||
          pos_beg >= pos_end) {
        return -1;
      }
      user_passwd_base64 = line.substr(pos_beg+1, pos_end-pos_beg-1);
      if (Base64Decode(user_passwd_base64, &user_passwd) != 0) return -1;
      auto pos = user_passwd.find(":");
      if (pos == std::string::npos) return -1;
      user = user_passwd.substr(0, pos);
      passwd = user_passwd.substr(pos+1, user_passwd.size()-pos);
    } else if (line.find("Position: ") != std::string::npos) {
      // Parse position from custom header: Position: lat=12.345678,lon=45.678901
      auto pos_beg = line.find(' ');
      std::string position_str = line.substr(pos_beg+1, line.size()-pos_beg-1);
      if (ParsePositionFromHeader(position_str, &latitude, &longitude) == 0) {
        has_position = true;
        printf("Base station position: lat=%.6f, lon=%.6f\n", latitude, longitude);
      }
    }
  }
  
  for (auto const& line : lines) {
    if (line.find("Ntrip-STR: ") != std::string::npos) {
      std::vector<std::string> sections;
      auto pos = line.find(' ');
      ntrip_str = line.substr(pos+1, line.size()-pos);
      StringSplit(ntrip_str, ";", &sections);
      if ((sections.size() > 4) &&
          (sections[1] != sections[2] || sections[1] != mount_point)) {
        return -1;
      }
      
      // Try to extract position from Ntrip-STR if not already found
      if (!has_position && sections.size() > 8) {
        // Ntrip-STR format: STR;mountpoint;mountpoint;carrier;nav;freq;auth;fee;bitrate;misc
        // Some implementations include lat,lon in the misc field
        std::string misc = sections.size() > 9 ? sections[9] : "";
        if (!misc.empty() && misc.find(',') != std::string::npos) {
          if (ParsePositionFromHeader(misc, &latitude, &longitude) == 0) {
            has_position = true;
            printf("Base station position from STR: lat=%.6f, lon=%.6f\n", latitude, longitude);
          }
        }
      }
    }
  }
  
  if (!mount_point.empty() && !user.empty() && !passwd.empty()) {
    MountPointInformation mount_point_info = {
        socket_fd, mount_point, user, passwd, {}, latitude, longitude, has_position};
    if (send(socket_fd, "HTTP/1.1 200 OK\r\n", 17, 0) == 17) {
      mount_point_infos_.push_back(mount_point_info);
      ntrip_str_list_.push_back(ntrip_str);
      printf("Base station registered: %s (has_position=%s)\n", 
             mount_point.c_str(), has_position ? "yes" : "no");
      return 0;
    }
  }
  return -1;
}

int NtripCaster::ClientConnectRequest(
    std::vector<std::string> const& lines, int socket_fd) {
  std::string mount_point;
  std::string user_passwd_base64;
  std::string user_passwd;
  std::string user;
  std::string passwd;
  bool ntrip_version_1 = false;
  double client_lat = 0.0;
  double client_lon = 0.0;
  bool has_client_position = false;
  
  for (auto const& line : lines) {
    if (line.find("GET") != std::string::npos) {
      if (line.find("HTTP/1.0") != std::string::npos) ntrip_version_1 = true;
      auto pos_beg = line.find('/');
      auto pos_end = line.find(' ', pos_beg);
      if (pos_beg == std::string::npos || pos_end == std::string::npos ||
          pos_beg >= pos_end) {
        break;
      }
      mount_point = line.substr(pos_beg+1, pos_end-pos_beg-1);
    } else if (line.find("Authorization: Basic") != std::string::npos) {
      auto pos_beg = line.find_last_of(' ');
      auto pos_end = line.find('\r', pos_beg);
      if (pos_beg == std::string::npos || pos_end == std::string::npos ||
          pos_beg >= pos_end) {
        break;
      }
      user_passwd_base64 = line.substr(pos_beg+1, pos_end-pos_beg-1);
      if (Base64Decode(user_passwd_base64, &user_passwd) == -1) break;
      auto pos = user_passwd.find(":");
      if (pos == std::string::npos) break;
      user = user_passwd.substr(0, pos);
      passwd = user_passwd.substr(pos+1, user_passwd.size()-pos);
    } else if (line.find("Position: ") != std::string::npos) {
      // Parse client position from custom header
      auto pos_beg = line.find(' ');
      std::string position_str = line.substr(pos_beg+1, line.size()-pos_beg-1);
      if (ParsePositionFromHeader(position_str, &client_lat, &client_lon) == 0) {
        has_client_position = true;
        printf("Client position from header: lat=%.6f, lon=%.6f\n", client_lat, client_lon);
      }
    }
  }
  
  if (!mount_point.empty() && !user.empty() && !passwd.empty()) {
    // Handle auto-selection
    if (mount_point == "auto") {
      if (!has_client_position) {
        printf("Auto-selection requested but no client position available\n");
        if (send(socket_fd, "HTTP/1.1 400 Bad Request\r\n", 25, 0) != 25) ;
        return -1;
      }
      
      // Find the closest base station
      double min_distance = std::numeric_limits<double>::max();
      MountPointInformation* best_mountpoint = nullptr;
      
      for (auto& info : mount_point_infos_) {
        if (info.has_position) {
          double distance = CalculateDistance(client_lat, client_lon, 
                                            info.latitude, info.longitude);
          printf("Distance to %s: %.2f meters\n", info.mountpoint.c_str(), distance);
          
          if (distance < min_distance) {
            min_distance = distance;
            best_mountpoint = &info;
          }
        }
      }
      
      if (best_mountpoint != nullptr) {
        printf("Auto-selected mountpoint: %s (distance: %.2f meters)\n", 
               best_mountpoint->mountpoint.c_str(), min_distance);
        
        // Check authentication for the selected mountpoint
        if (user == best_mountpoint->username && passwd == best_mountpoint->password) {
          std::string response = "HTTP/1.1 200 OK\r\n";
          if (ntrip_version_1) response = "ICY 200 OK\r\n";
          int len = response.size();
          if (send(socket_fd, response.c_str(), len, 0) == len) {
            best_mountpoint->client_socket_list.push_back(socket_fd);
            return 0;
          }
        } else {
          printf("Authentication failed for auto-selected mountpoint: %s\n", 
                 best_mountpoint->mountpoint.c_str());
        }
      } else {
        printf("No base stations with position data available for auto-selection\n");
        if (send(socket_fd, "HTTP/1.1 503 Service Unavailable\r\n", 31, 0) != 31) ;
        return -1;
      }
    } else {
      // Standard mountpoint selection
      for (auto& info : mount_point_infos_) {
        if ((mount_point == info.mountpoint) && (user == info.username) &&
            (passwd == info.password)) {
          std::string response = "HTTP/1.1 200 OK\r\n";
          if (ntrip_version_1) response = "ICY 200 OK\r\n";
          int len = response.size();
          if (send(socket_fd, response.c_str(), len, 0) == len) {
            info.client_socket_list.push_back(socket_fd);
            return 0;
          }
        }
      }
    }
  }
  
  if (send(socket_fd, "HTTP/1.1 401 Unauthorized\r\n", 27, 0) != 27) ;
  return -1;
}

}  // namespace libntrip
