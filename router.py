import socket
import argparse


def router_main():
    argparser = argparse.ArgumentParser()
    argparser.add_argument('-p', '--port', type=int, default=3333)
    argparser.add_argument('-i', '--ip', type=str, default='127.0.0.1')
    argparser.add_argument('-rp', '--remote-port', type=int, default=4321)
    argparser.add_argument('-ri', '--remote-ip', type=str, default='127.0.0.1')
    argparser.add_argument('-l', '--loss', type=float, default=0.03)
    argparser.add_argument('-d', '--delay', type=float, default=5)

    args = argparser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.ip, args.port))
    print('router is running on {}:{}'.format(args.ip, args.port))

    count = 1
    loss_period = int(1 / args.loss - 1)

    print('loss period is {}'.format(loss_period))
    
    client_addr = None

    while True:
        data, addr = sock.recvfrom(65535)
        if addr[0] != args.remote_ip or addr[1] != args.remote_port:
            if client_addr is None:
                client_addr = addr
                print('client address is {}'.format(client_addr))

            if count % loss_period == 0:
                print('loss packet')
                count = 1
                continue

            print('delay packet')
            count += 1
            sock.sendto(data, (args.remote_ip, args.remote_port))
        else:
            sock.sendto(data, client_addr)


if __name__ == '__main__':
    router_main()
