#include "network.h"

#include "connection.h"
#include "server.h"

namespace kim {

Network::Network(Log* logger, TYPE type) : m_type(type) {
}

Network::~Network() {
    destory();
}

void Network::destory() {
}

void Network::run() {
    co_eventloop(co_get_epoll_ct(), 0, 0);
}

bool Network::set_gate_codec(const std::string& codec_type) {
    Codec::TYPE type = Codec::get_codec_type(codec_type);
    if (type != Codec::TYPE::UNKNOWN) {
        m_gate_codec = type;
        return true;
    }
    return false;
}

int Network::listen_to_port(const char* host, int port) {
    int fd = -1;
    char errstr[256];

    fd = anet_tcp_server(errstr, host, port, TCP_BACK_LOG);
    if (fd == -1) {
        LOG_ERROR("bind tcp ipv4 failed! %s", errstr);
        return -1;
    }

    if (anet_no_block(m_errstr, fd) != ANET_OK) {
        LOG_ERROR("set socket no block failed! fd: %d, errstr: %s", fd, m_errstr);
        close_fd(fd);
        return -1;
    }

    LOG_INFO("listen to port, %s:%d", host, port);
    return fd;
}

void Network::close_fd(int fd) {
    if (close(fd) == -1) {
        LOG_WARN("close channel failed, fd: %d. errno: %d, errstr: %s",
                 fd, errno, strerror(errno));
    }
    LOG_DEBUG("close fd: %d.", fd);
}

void* Network::co_handler_accept_nodes_conn(void*) {
    co_enable_hook_sys();

    for (;;) {
    }

    return 0;
}

void* Network::co_handler_accept_gate_conn(void* d) {
    co_enable_hook_sys();
    Connection* c = (Connection*)d;
    Network* net = (Network*)c->privdata();
    return net->handler_accept_gate_conn(d);
}

/* 创建新的子协程，创建新的 connection. */
void* Network::handler_accept_gate_conn(void* d) {
    channel_t ch;
    chanel_resend_data_t* ch_data;
    char ip[NET_IP_STR_LEN] = {0};
    int fd, port, family, chanel_fd, err;

    for (;;) {
        fd = anet_tcp_accept(m_errstr, m_gate_host_fd, ip, sizeof(ip), &port, &family);
        if (fd == ANET_ERR) {
            if (errno != EWOULDBLOCK) {
                LOG_WARN("accepting client connection: %s", m_errstr);
            }
            struct pollfd pf = {0};
            pf.fd = -1;
            poll(&pf, 1, 1000);
            continue;
        }

        LOG_INFO("accepted client: %s:%d, fd: %d", ip, port, fd);

        chanel_fd = m_worker_data_mgr->get_next_worker_data_fd();
        if (chanel_fd <= 0) {
            LOG_ERROR("find next worker chanel failed!");
            close_fd(fd);
            continue;
        }

        LOG_TRACE("send client fd: %d to worker through chanel fd %d", fd, chanel_fd);

        ch = {fd, family, static_cast<int>(m_gate_codec), 0};
        err = write_channel(chanel_fd, &ch, sizeof(channel_t), m_logger);
        if (err != 0) {
            if (err == EAGAIN) {
                /* re send again in timer. */
                ch_data = (chanel_resend_data_t*)malloc(sizeof(chanel_resend_data_t));
                memset(ch_data, 0, sizeof(chanel_resend_data_t));
                ch_data->ch = ch;
                m_wait_send_fds.push_back(ch_data);
                LOG_TRACE("wait to write channel, errno: %d", err);
                continue;
            }
            LOG_ERROR("write channel failed! errno: %d", err);
        }

        close_fd(fd);
        continue;
    }

    return 0;
}

void* Network::co_handler_requests(void*) {
    co_enable_hook_sys();

    for (;;) {
    }

    return 0;
}

Connection* Network::create_conn(int fd) {
    auto it = m_conns.find(fd);
    if (it != m_conns.end()) {
        LOG_WARN("find old connection, fd: %d", fd);
        // close_conn(fd);
    }

    uint64_t seq;
    Connection* c;

    seq = new_seq();
    c = new Connection(m_logger, fd, seq);
    if (c == nullptr) {
        LOG_ERROR("new connection failed! fd: %d", fd);
        return nullptr;
    }

    m_conns[fd] = c;
    c->set_keep_alive(m_keep_alive);
    LOG_DEBUG("create connection fd: %d, seq: %llu", fd, seq);
    return c;
}

Connection* Network::create_conn(int fd, Codec::TYPE codec, bool is_chanel) {
    if (anet_no_block(m_errstr, fd) != ANET_OK) {
        LOG_ERROR("set socket no block failed! fd: %d, errstr: %s", fd, m_errstr);
        return nullptr;
    }

    if (!is_chanel) {
        if (anet_keep_alive(m_errstr, fd, 100) != ANET_OK) {
            LOG_ERROR("set socket keep alive failed! fd: %d, errstr: %s", fd, m_errstr);
            return nullptr;
        }
        if (anet_set_tcp_no_delay(m_errstr, fd, 1) != ANET_OK) {
            LOG_ERROR("set socket no delay failed! fd: %d, errstr: %s", fd, m_errstr);
            return nullptr;
        }
    }

    Connection* c = create_conn(fd);
    if (c == nullptr) {
        close_fd(fd);
        LOG_ERROR("add chanel event failed! fd: %d", fd);
        return nullptr;
    }
    c->init(codec);
    c->set_privdata(this);
    c->set_active_time(now());
    c->set_state(Connection::STATE::CONNECTED);

    if (is_chanel) {
        c->set_system(true);
    }

    LOG_TRACE("create connection done! fd: %d", fd);
    return c;
}

/* parent. */
bool Network::create_m(const addr_info* ai, const CJsonObject& config) {
    if (ai == nullptr) {
        return false;
    }

    int fd = -1;
    Connection* c;

    if (!ai->gate_host().empty()) {
        fd = listen_to_port(ai->gate_host().c_str(), ai->gate_port());
        if (fd == -1) {
            LOG_ERROR("listen to gate failed! %s:%d",
                      ai->gate_host().c_str(), ai->gate_port());
            return false;
        }

        m_gate_host_fd = fd;
        m_gate_host = ai->gate_host();
        m_gate_port = ai->gate_port();
        LOG_INFO("gate fd: %d", m_gate_host_fd);

        c = create_conn(m_gate_host_fd, m_gate_codec);
        if (c == nullptr) {
            close_fd(m_gate_host_fd);
            LOG_ERROR("add read event failed, fd: %d", m_gate_host_fd);
            return false;
        }

        /* co_accecpt_node_conns */
        stCoRoutine_t* co_accecpt_gate_conns;
        co_create(&co_accecpt_gate_conns, NULL, co_handler_accept_gate_conn, (void*)c);
        co_resume(co_accecpt_gate_conns);
    }

    return true;
}

/* worker. */
bool Network::create_w(const CJsonObject& config, int ctrl_fd, int data_fd, int index) {
    return true;
}

}  // namespace kim