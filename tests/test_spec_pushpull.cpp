/*
    Copyright (c) 2007-2013 Contributors as noted in the AUTHORS file

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include "testutil.hpp"

void test_push_round_robin_out (void *ctx)
{
    void *push = zmq_socket (ctx, ZMQ_PUSH);
    assert (push);

    int rc = zmq_bind (push, "inproc://b");
    assert (rc == 0);

    const size_t N = 5;
    void *pulls[N];
    for (size_t i = 0; i < N; ++i)
    {
        pulls[i] = zmq_socket (ctx, ZMQ_PULL);
        assert (pulls[i]);

        int timeout = 100;
        rc = zmq_setsockopt (pulls[i], ZMQ_RCVTIMEO, &timeout, sizeof(int));
        assert (rc == 0);

        rc = zmq_connect (pulls[i], "inproc://b");
        assert (rc == 0);
    }

    // Send 2N messages
    for (size_t i = 0; i < N; ++i)
    {
        s_send_seq (push, "ABC", SEQ_END);
    }
    for (size_t i = 0; i < N; ++i)
    {
        s_send_seq (push, "DEF", SEQ_END);
    }

    // Expect every PULL got one of each
    for (size_t i = 0; i < N; ++i)
    {
        s_recv_seq (pulls[i], "ABC", SEQ_END);
        s_recv_seq (pulls[i], "DEF", SEQ_END);
    }

    rc = zmq_close (push);
    assert (rc == 0);

    for (size_t i = 0; i < N; ++i)
    {
        rc = zmq_close (pulls[i]);
        assert (rc == 0);
    }
}

void test_pull_fair_queue_in (void *ctx)
{
    void *pull = zmq_socket (ctx, ZMQ_PULL);
    assert (pull);

    int rc = zmq_bind (pull, "inproc://a");
    assert (rc == 0);

    const size_t N = 5;
    void *pushs[N];
    for (size_t i = 0; i < N; ++i)
    {
        pushs[i] = zmq_socket (ctx, ZMQ_PUSH);
        assert (pushs[i]);

        rc = zmq_connect (pushs[i], "inproc://a");
        assert (rc == 0);
    }

    // Send 2N messages
    for (size_t i = 0; i < N; ++i)
    {
        char * str = strdup("A");
        str[0] += i;
        s_send_seq (pushs[i], str, SEQ_END);
        str[0] += N;
        s_send_seq (pushs[i], str, SEQ_END);
        free (str);
    }

    // Expect to pull them in order
    for (size_t i = 0; i < 2*N; ++i)
    {
        char * str = strdup("A");
        str[0] += i;
        s_recv_seq (pull, str, SEQ_END);
        free (str);
    }

    rc = zmq_close (pull);
    assert (rc == 0);

    for (size_t i = 0; i < N; ++i)
    {
        rc = zmq_close (pushs[i]);
        assert (rc == 0);
    }
}

void test_push_block_on_send_no_peers (void *ctx)
{
    void *sc = zmq_socket (ctx, ZMQ_PUSH);
    assert (sc);

    int timeout = 100;
    int rc = zmq_setsockopt (sc, ZMQ_SNDTIMEO, &timeout, sizeof(timeout));
    assert (rc == 0);

    rc = zmq_send (sc, 0, 0, ZMQ_DONTWAIT);
    assert (rc == -1);
    assert (errno == EAGAIN);

    rc = zmq_send (sc, 0, 0, 0);
    assert (rc == -1);
    assert (errno == EAGAIN);

    rc = zmq_close (sc);
    assert (rc == 0);
}

void test_destroy_queue_on_disconnect (void *ctx)
{
    void *A = zmq_socket (ctx, ZMQ_PUSH);
    assert (A);

    int hwm = 1;
    int rc = zmq_setsockopt (A, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    assert (rc == 0);

    rc = zmq_bind (A, "inproc://d");
    assert (rc == 0);

    void *B = zmq_socket (ctx, ZMQ_PULL);
    assert (B);

    rc = zmq_setsockopt (B, ZMQ_RCVHWM, &hwm, sizeof(hwm));
    assert (rc == 0);

    rc = zmq_connect (B, "inproc://d");
    assert (rc == 0);

    // Send two messages, one should be stuck in A's outgoing queue, the other
    // arrives at B.
    s_send_seq (A, "ABC", SEQ_END);
    s_send_seq (A, "DEF", SEQ_END);

    // Both queues should now be full, indicated by A blocking on send.
    rc = zmq_send (A, 0, 0, ZMQ_DONTWAIT);
    assert (rc == -1);
    assert (errno == EAGAIN);

    rc = zmq_disconnect (B, "inproc://d");
    assert (rc == 0);

    // Disconnect may take time and need command processing.
    zmq_pollitem_t poller[2] = { { A, 0, 0, 0 }, { B, 0, 0, 0 } };
    rc = zmq_poll (poller, 2, 100);
    assert (rc == 0);

    zmq_msg_t msg;
    rc = zmq_msg_init (&msg);
    assert (rc == 0);

    // Can't receive old data on B.
    rc = zmq_msg_recv (&msg, B, ZMQ_DONTWAIT);
    assert (rc == -1);
    assert (errno == EAGAIN);

    // Sending fails.
    rc = zmq_send (A, 0, 0, ZMQ_DONTWAIT);
    assert (rc == -1);
    assert (errno == EAGAIN);

    // Reconnect B
    rc = zmq_connect (B, "inproc://d");
    assert (rc == 0);

    // Still can't receive old data on B.
    rc = zmq_msg_recv (&msg, B, ZMQ_DONTWAIT);
    assert (rc == -1);
    assert (errno == EAGAIN);

    // two messages should be sendable before the queues are filled up.
    s_send_seq (A, "ABC", SEQ_END);
    s_send_seq (A, "DEF", SEQ_END);

    rc = zmq_send (A, 0, 0, ZMQ_DONTWAIT);
    assert (rc == -1);
    assert (errno == EAGAIN);

    rc = zmq_msg_close (&msg);
    assert (rc == 0);

    rc = zmq_close (A);
    assert (rc == 0);

    rc = zmq_close (B);
    assert (rc == 0);
}

int main ()
{
    void *ctx = zmq_ctx_new ();
    assert (ctx);

    // PUSH: SHALL route outgoing messages to connected peers using a
    // round-robin strategy.
    test_push_round_robin_out (ctx);

    // PULL: SHALL receive incoming messages from its peers using a fair-queuing
    // strategy.
    test_pull_fair_queue_in (ctx);

    // PUSH: SHALL block on sending, or return a suitable error, when it has no
    // available peers.
    test_push_block_on_send_no_peers (ctx);

    // PUSH and PULL: SHALL create this queue when a peer connects to it. If
    // this peer disconnects, the socket SHALL destroy its queue and SHALL
    // discard any messages it contains.
    test_destroy_queue_on_disconnect (ctx);

    int rc = zmq_ctx_term (ctx);
    assert (rc == 0);

    return 0 ;
}
