//////////////////////////
// Эхо сервер на libev  //
//////////////////////////
#include <stdio.h>
#include <netinet/in.h>
#include <ev.h>
int main(int argc, char **argv) {
   // Создаём основной цикл (в loop возвращается адрес на структуру в куче).
   struct ev_loop *loop = ev_default_loop(0);
   // Создаём серверный сокет.
   int sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   // Задаём IP-адрес и порт.
   struct sockaddr_in addr;
   bzero(&addr,sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = htons(12345);
   addr.sin_addr.s_addr = htonl(INADDR_ANY);
   // Адрес присоединяем сокету.
   bind(sd, (struct sockaddr *)&addr, sizeof(addr));
   // Переводим сокет в режим прослушивания входящих соединений.
   listen(sd, SOMAXCONN);
   // Создаём вотчер для отслеживания события - На сокете сервера доступны данные для чтения
   // (можно прочитать дескриптор присоединившегося сокета клиента).
   struct ev_io w_accept;
   ev_io_init(&w_accept, cb_accept, sd, EV_READ);
   // Добавляем созданный вотчер в цикл loop.
   ev_io_start(loop, &w_accept);
   // Вечный цикл.
   while(1) {
      // Старт цикла loop (в этом месте система начинает ждать события, согласно зарегистрированных вотчеров);
      ev_loop(loop, 0);
   }
}
// Если вотчеры создаются в функциях CALLBACK, то вотчер должен выдёляться в куче, чтобы при выходе 
// цикл loop смог иметь доступ к вотчеру, адрес которого размещён не на стеке, а в куче.
static void cp_accept(stuct ev_loop *loop, stuct ev_io *watcher, int revents){
// CALLBACK - на серверный сокет поступил запрос от клиента.
   // Получаем клиентский сокет.
   int client_sd = accept(watcher->fd, 0, 0);
   // Поймав первое событие, например на Master-сокете, получили Slave-сокет,
   // создаём вотчер для отслеживания события на Slave-сокете.
   // Структуру вотчера создаём в куче, для отслеживания событий доступно чтение с сокета клиента.
   struct ev_io *w_client = (struct ev_io*)malloc(sizeof(struct ev_io));
   ev_io_init(w_client, cb_read_data_client, client_sd, EV_READ);
   // Регистрируем вотчер в лупе (добавляем вотчер в loop).
   ev_io_start(loop, w_client);

}
static void cb_read_data_client(stuct ev_loop *loop, stuct ev_io *watcher, int revents){
// CALLBACK - на сокете клиента появились данные для чтения.
   char buf[1024];
   ssize_t r = recv(watcher->fd, buf, 1024, MSG_NOSIGNAL);
   // Если ошибка выходим из обработчика.
   if(r < 0) {      
      return;
   // Если прочитали 0 байт, то соединение закрылось, клиент закрыл свой сокет на своей стороне.
   } else if(r == 0){
      // Удаляем наш вотчер (опрос события на этом дескрипторе сокета клиента) из цикла.
      ev_io_stop(loop, watcher);
      // Освобождаем память, которую занимает данный вотчер в куче.
      free(watcher);
      return;
   // Если всё в порядке.
   } else {
      // Отправляем Эхо-ответ клиенту.
      send(watcher->fd, buf, r, MSG_NOSIGNAL);
   }
}
