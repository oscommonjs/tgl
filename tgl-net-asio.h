/*
    This file is part of tgl-library

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

    Copyright Vitaly Valtman 2013-2015
    Copyright Topology LP 2016
*/

#ifndef __TGL_NET_ASIO_H__
#define __TGL_NET_ASIO_H__

#include "tgl-net.h"

#include <boost/asio.hpp>
#include <mutex>

struct connection_buffer {
    unsigned char *start;
    unsigned char *end;
    unsigned char *rptr;
    unsigned char *wptr;
    struct connection_buffer *next;
};

enum conn_state {
    conn_none,
    conn_connecting,
    conn_ready,
    conn_failed,
    conn_stopped
};

struct tgl_dc;
struct tgl_session;

class tgl_connection_asio : public std::enable_shared_from_this<tgl_connection_asio>
        , public tgl_connection
{
public:
    tgl_connection_asio(boost::asio::io_service& io_service,
            const std::string& host,
            int port,
            const std::weak_ptr<tgl_session>& session,
            const std::weak_ptr<tgl_dc>& dc,
            const std::shared_ptr<mtproto_client>& client);
    virtual ~tgl_connection_asio();

    virtual bool open() override;
    virtual void close() override;
    virtual ssize_t read(void* buffer, size_t len) override;
    virtual ssize_t write(const void* data, size_t len) override;
    virtual void flush() override;
    virtual const std::weak_ptr<tgl_dc>& get_dc() const override { return m_dc; }
    virtual const std::weak_ptr<tgl_session>& get_session() const override { return m_session; }
    virtual void incr_out_packet_num() override { m_out_packet_num++; }

    bool connect();
    void restart();
    void fail();

    void start_ping_timer();

private:
    void start_read();
    void handle_read(const boost::system::error_code&, size_t);

    void start_write();
    void handle_write(const boost::system::error_code&, size_t);

    void stop_ping_timer();
    void ping_alarm(const boost::system::error_code&);

    void start_fail_timer();
    void fail_alarm(const boost::system::error_code&);

    ssize_t read_in_lookup(void *data, size_t len);
    void try_rpc_read();

    void handle_connect(const boost::system::error_code&);
    void free_buffers();

    bool m_closed;

    std::string m_ip;
    int m_port;
    enum conn_state m_state;
    boost::asio::io_service& m_io_service;
    boost::asio::ip::tcp::socket m_socket;
    boost::asio::deadline_timer m_ping_timer;
    boost::asio::deadline_timer m_fail_timer;

    size_t m_out_packet_num;
    connection_buffer* m_in_head;
    connection_buffer* m_in_tail;
    connection_buffer* m_out_head;
    connection_buffer* m_out_tail;
    size_t m_in_bytes;
    size_t m_bytes_to_write;
    std::weak_ptr<tgl_dc> m_dc;
    std::weak_ptr<tgl_session> m_session;
    std::shared_ptr<mtproto_client> m_mtproto_client;

    double m_last_connect_time;
    double m_last_receive_time;

    bool m_in_fail_timer;
    bool m_write_pending;

    // FIXME: remove this when we change the out buffer management.
    std::recursive_mutex m_mutex;
};

class tgl_connection_factory_asio : public tgl_connection_factory
{
public:
    explicit tgl_connection_factory_asio(boost::asio::io_service& io_service)
        : m_io_service(io_service)
    { }

    virtual std::shared_ptr<tgl_connection> create_connection(
            const std::string& host,
            int port,
            const std::weak_ptr<tgl_session>& session,
            const std::weak_ptr<tgl_dc>& dc,
            const std::shared_ptr<mtproto_client>& client) override
    {
        return std::make_shared<tgl_connection_asio>(m_io_service,
                host, port, session, dc, client);
    }

private:
    boost::asio::io_service& m_io_service;
};

#endif
