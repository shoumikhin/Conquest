#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <termios.h>
#include <fcntl.h>
#include <math.h>

#ifndef INADDR_NONE
#define INADDR_NONE 0xFFFFFFFF
#endif

#define BUFFER_SIZE 0x1000  //must be at least bigger than 0x100
#define MAX_PLAYERS 10
#define NAME_LENGTH 20
#define COMMAND_LENGTH 0x100
#define SHIPS_DEFAULT 10

#define SIGNATURE 0xCE
#define REFUSE 0xEE
#define OK 0xBF

#define EXIT 0xEC
#define MAP 0xAD
#define PLANET 0xAE
#define WHO 0xEF
#define GO 0xAA
#define END 0xED
#define MESSAGE 0xEA
#define LOOSE 0xBA
#define YOU_WIN 0xCB
#define NEXT_TURN 0xCC

#define LT '/'
#define LB '\\'
#define RT '\\'
#define RB '/'
#define HB '-'
#define VB '|'

#include "routines.h"

//------------------------------------------------------------------------------
struct
{
	int width, height, planets, players;

	struct planet
	{
		char name;
		int owner;
		unsigned ships;
		unsigned short product;
	} *planets_data;

	struct player
	{
		char name[NAME_LENGTH];
		int id;
		pthread_mutex_t turn;
		pthread_mutex_t sync;
	} *players_data;

	struct transaction
	{
		char from;
		char to;
		int number;
		char i,j;  //destination coords
		int owner;
		int delay;
		struct transaction *next;
	} *transactions_data;
} game;
//------------------------------------------------------------------------------
pthread_t server_console;
pthread_mutex_t game_mutex;
pthread_mutex_t turn_mutex;
int turn;
char alphabet[] = {'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z','0','1','2','3','4','5','6','7','8','9'};

char str_broadcast[BUFFER_SIZE - 50];  //just smaller than size of buffer (no hint)
char str_broadcast_adder[0x100];
char str_broadcast_helper[0x100];
char str_no_memory[] = "No memory\n";
char str_server_disconnected[] = "Server disconnected\n";
char str_go_not_exist_destination[] = "A destination planet does not exist\n";
char str_go_not_exist_source[] = "A source planet does not exist\n";
char str_go_not_owner[] = "You are not an owner of the source planet\n";
char str_go_not_enought[] = "Not enought ships on the source planet\n";
//------------------------------------------------------------------------------
int errexit(char const *format, ...)
{
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);

	exit(1);
}
//------------------------------------------------------------------------------
int connectsock(char const *service, char const *host)
{
	struct hostent *phe;
	struct servent *pse;
	struct protoent *ppe;
	struct sockaddr_in sin;
	int s, type;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;

	if (pse = getservbyname(service, "tcp"))
		sin.sin_port = pse->s_port;
	else if (!(sin.sin_port = htons((unsigned short)atoi(service))))
			errexit("Cannot get \"%s\" service entry\n", service);

	if (phe = gethostbyname(host))
		memcpy(&sin.sin_addr, phe->h_addr, phe->h_length);
	else if ((sin.sin_addr.s_addr = inet_addr(host)) == INADDR_NONE)
			errexit("Cannot get \"%s\" host entry", host);

	if (!(ppe = getprotobyname("tcp")))
		errexit("Cannot get TCP protocol entry\n");

	type = SOCK_STREAM;

	s = socket(PF_INET, type, ppe->p_proto);

	if (s < 0)
		errexit("Cannot create a socket : %s\n", strerror(errno));

	if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) < 0)
		errexit("Cannot connect to %s:%s : %s\n", host, service, strerror(errno));

	return s;
}
//------------------------------------------------------------------------------
int passivesock(char const *service, int qlen)
{
	struct servent *pse;
	struct protoent *ppe;
	struct sockaddr_in sin;
	int on = 1;

	memset(&sin, 0, sizeof(sin));
	int s, type;
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = INADDR_ANY;

	if (pse = getservbyname(service, "tcp"))
		sin.sin_port = htons(ntohs((unsigned short)pse->s_port));
	else if (!(sin.sin_port = htons((unsigned short)atoi(service))))
			errexit("Cannot get \"%s\" service entry\n", service);

	if (!(ppe = getprotobyname("tcp")))
		errexit("Cannot get TCP protocol entry\n");

	type = SOCK_STREAM;

	s = socket(PF_INET, type, ppe->p_proto);

	if (s < 0)
		errexit("Cannot create socket : %s\n", strerror(errno));

	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void *)&on, sizeof(on)) < 0)
		errexit("Cannot set socket options : %s\n", strerror(errno));

	if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0)
		errexit("Cannot bind to %s port : %s\n", service, strerror(errno));

	listen(s, qlen);

	return s;
}
//------------------------------------------------------------------------------
void clearInput()
{
	struct termios original_mode, new_mode;
	int original_flag, new_flag;

	tcgetattr(0, &original_mode);
	original_flag = fcntl(0, F_GETFL);

	tcgetattr(0, &new_mode);
	new_mode.c_lflag &= ~ICANON;
	new_mode.c_lflag &= ~ECHO;
	new_mode.c_cc[VMIN] = 1;
	tcsetattr(0, TCSANOW, &new_mode);
	new_flag = fcntl(0, F_GETFL);
	new_flag |= O_NDELAY;
	fcntl(0, F_SETFL, new_flag);

	while (EOF != getchar());

	tcsetattr(0, TCSANOW, &original_mode);
	fcntl(0, F_SETFL, original_flag);
}
//------------------------------------------------------------------------------
void inputParameter(char *prompt, int *param, int l, int r)
{
	char input[64], *endptr;
	long val;

	printf("%s [%i]: ", prompt, *param);
	fgets(input, sizeof(input), stdin);
	val = strtol(input, &endptr, 10);

    if (input != endptr)
    {
        if (val < l)  *param = l;
        else if (val > r)  *param = r;
            else *param = val;
    }

    if (game.height*game.width < game.planets)
        game.planets = game.height*game.width;

    if (game.planets < game.players)
        game.players = game.planets;

    printf("Accepted %i\n", *param);
}
//------------------------------------------------------------------------------
void initGame()
{
	int k;

	pthread_mutex_init(&game_mutex, NULL);
	pthread_mutex_init(&turn_mutex, NULL);

	game.height = 10;
	game.width = 10;
	game.planets = 10;
	game.players = 2;

    inputParameter("Input map height", &game.height, 2, 25);
    inputParameter("Input map width", &game.width, 2, 25);
    inputParameter("Input the number of planets", &game.planets, 2, sizeof(alphabet));
    inputParameter("Input the limit of players", &game.players, 2, MAX_PLAYERS);

    game.planets_data = (struct planet *)malloc(sizeof(struct planet) * game.width * game.height);

    if (!game.planets_data) errexit(str_no_memory);

    game.players_data = (struct player *)malloc(sizeof(struct player) * game.players);

    if (!game.players_data)
    {
        free(game.planets_data);

        errexit(str_no_memory);
    }

    memset(game.planets_data, 0, sizeof(struct planet) * game.width * game.height);
    memset(game.players_data, 0, sizeof(struct player) * game.players);

    game.transactions_data = NULL;

    turn = 1;
}
//------------------------------------------------------------------------------
void initPlanets()
{
    int i, j, k, o;

    srand(time(NULL));

    for (k = o = 0; k < game.planets;)
    {
        i = rand() % game.height;
        j = rand() % game.width;

        if (game.planets_data[i * game.width + j].name & 0x80) continue;  //highest bit in name means planet (not empty place)

        game.planets_data[i * game.width + j].name = alphabet[k] | 0x80;
        ++k;

        if (o < game.players)
        {
            game.planets_data[i * game.width + j].owner = game.players_data[o].id;
            game.planets_data[i * game.width + j].ships = SHIPS_DEFAULT;
            game.planets_data[i * game.width + j].product = SHIPS_DEFAULT;
            ++o;
        }
        else
        {
            game.planets_data[i * game.width + j].owner = 0;
            game.planets_data[i * game.width + j].ships = SHIPS_DEFAULT - SHIPS_DEFAULT / 2 + rand() % SHIPS_DEFAULT + 1;
            game.planets_data[i * game.width + j].product = SHIPS_DEFAULT - SHIPS_DEFAULT / 2 + rand() % SHIPS_DEFAULT + 1;
        }
    }
}
//------------------------------------------------------------------------------
void help()
{
	printf("\n\"exit\" - Quit the game\n"
		   "\"help\" - List available commands\n"
		   "\"map\" - Dump the space map\n"
		   "\"<name>\" - Show info about planet named <name> (<name> is a character)\n"
		   "\"who\" - List players and their planets\n"
		   "\"go <from> <to> <number>\" - Move (integer) <number> of ships from planet <from> to planet <to> (characters)\n"
		   "\"end\" - End turn and wait for others\n\n"
		  );
}
//------------------------------------------------------------------------------
void dumpMap()
{
	int i, j, k, length;

	pthread_mutex_lock(&game_mutex);

	length = 2 * game.width - 1;

	putchar('\n');
	putchar(LT);

	for (k = 0; k < length; ++k) putchar(HB);

	putchar(RT);
	putchar('\n');

	for (i = 0; i < game.height; ++i)
	{
		putchar(VB);

		for (j = 0; j < game.width; ++j)
		{
			if (game.planets_data[i * game.width + j].name & 0x80)
				putchar(game.planets_data[i * game.width + j].name & 0x7F);
			else
				putchar(' ');

			putchar(VB);
		}

		putchar('\n');

		if (i != game.height - 1)
		{
			putchar(VB);

			for (k = 0; k < game.width - 1; ++k)
			{
				putchar(HB);
				putchar('+');
			}

			putchar(HB);
			putchar(VB);
			putchar('\n');
		}
	}

	putchar(LB);

	for (k = 0; k < length; ++k) putchar(HB);

	putchar(RB);
	putchar('\n');
	putchar('\n');

	pthread_mutex_unlock(&game_mutex);
}
//------------------------------------------------------------------------------
void dumpPlanet(char name)  //output info about a planet <name>
{
	int i, j, size;
	char found = 0;

	pthread_mutex_lock(&game_mutex);

	name = toupper(name);
	size = game.width * game.height;
	name |= 0x80;

	for (i = 0; i < size; ++i)
		if (game.planets_data[i].name == name)
		{
			found = 1;
			putchar('\n');
			printf("About planet %c\n", name & 0x7F);

			if (game.planets_data[i].owner)
			{
				for (j = 0; j < game.players; ++j)
					if (game.players_data[j].id == game.planets_data[i].owner)
					{
						printf("Owner: \"%s\"\n", game.players_data[j].name);
						printf("Ships amount: %i\n", game.planets_data[i].ships);
						printf("Production: %i\n", game.planets_data[i].product);

						break;
					}
			}
			else
			{
				printf("Owner: None\n");
				printf("Ships amount: Unknown\n");
				printf("Production: Unknown\n");
			}

			putchar('\n');

			break;
		}

	if (!found) printf("Planet %c does not exist\n", name & 0x7F);

	fflush(stdout);

	pthread_mutex_unlock(&game_mutex);
}
//------------------------------------------------------------------------------
void dumpUsers()
{
	int i, j, k;

	pthread_mutex_lock(&game_mutex);

	putchar('\n');
	fputs("Players:\n", stdout);

	for (k = 0; k < game.players; ++k)
		if (game.players_data[k].id)
		{
			printf("%-20s", game.players_data[k].name);

			fputs(": { ", stdout);

			for (i = 0; i < game.height; ++i)
				for (j = 0; j < game.width; ++j)
					if (game.planets_data[i * game.height + j].owner && game.planets_data[i * game.height + j].owner == game.players_data[k].id)
						printf("%c ", game.planets_data[i * game.height + j].name & 0x7F);

			fputs("}\n", stdout);
		}

	putchar('\n');
	fflush(stdout);

	pthread_mutex_unlock(&game_mutex);
}
//------------------------------------------------------------------------------
unsigned char console(void **op)
{
	char command[COMMAND_LENGTH], *parser;
	int length;

	for (;;)
	{
		fflush(stderr);
		printf("command> ");
		clearInput();
		fflush(stdout);

		if (!fgets(command, COMMAND_LENGTH, stdin))
		{
			fprintf(stderr, "Input error : %s\n", strerror(errno));

			break;
		}

		length = strlen(command);

		if (length > 1)
		{
			if ('\n' == command[length - 1]) command[length - 1] = '\0';
		}
		else
			continue;

		--length;

		if (!strcmp("map", command)) return MAP;

		if (1 == length)
		{
			*op = malloc(1);
			memcpy(*op, command, 1);

			return PLANET;
		}

		if (!strcmp("who", command)) return WHO;

		if (strstr(command, "go ") == command)
			if (8 <= length && command[4] == ' ' && command[6] == ' ' && !(command[7] < '1' || command[7] > '9'))  //8 == strlen("go X Y 1")
			{
				*op = malloc(length - 2);
				memcpy(*op, command + 3, length - 2);  //3 == strlen("go ")

				return GO;
			}
			else
			{
				printf("Incorrect go command \"%s\" (try \"help\")\n", command);

				continue;
			}

		if (!strcmp("end", command)) return END;

		if (!strcmp("exit", command)) return EXIT;

		if (!strcmp("help", command))
		{
			help();

			continue;
		};

		printf("Unknown command \"%s\" (try \"help\")\n", command);
	}
}
//------------------------------------------------------------------------------
char pushTransaction(int owner, char from, char to, int num)
{
	int i, j, ii, jj;
	char flag1 = 0, flag2 = 0, flag3 = 0;
	struct transaction *new, *cur;

	pthread_mutex_lock(&game_mutex);

	from = toupper(from);
	to = toupper(to);
	from |= 0x80;
	to |= 0x80;

	for (i = 0; i < game.height; ++i)
		for (j = 0; j < game.width; ++j)
			if (game.planets_data[i * game.height + j].name == to)
				{
					ii = i;
					jj = j;
					flag1 = 1;

					break;
				}

	if (!flag1)
	{
		pthread_mutex_unlock(&game_mutex);

		return 1;
	}

	flag1 = 0;

	for (i = 0; i < game.height; ++i)
		for (j = 0; j < game.width; ++j)
			if (game.planets_data[i * game.height + j].name == from)
			{
				flag1 = 1;

				if (game.planets_data[i * game.height + j].owner == owner)
				{
					flag2 = 1;

					if (from && game.planets_data[i * game.height + j].ships >= num)
					{
						flag3 = 1;

						new = (struct transaction *)malloc(sizeof(struct transaction));

						if (!new)
							fprintf(stderr, str_no_memory);
						else
						{
							game.planets_data[i * game.height + j].ships -= num;

							new->from = from;
							new->to = to;
							new->number = num;
							new->i = ii;
							new->j = jj;
							new->owner = owner;
							new->delay = floor(sqrt((i - ii) * (i - ii) + (j - jj) * (j - jj)) + 0.5);
							new->next = NULL;

							if (game.transactions_data)
							{
								cur = game.transactions_data;

								while (cur->next) cur = cur->next;

								cur->next = new;
							}
							else
								game.transactions_data = new;
						}

						goto out;
					}
				}
			}

out:

	pthread_mutex_unlock(&game_mutex);

	if (!flag1) return 2;

	if (!flag2) return 3;

	if (!flag3) return 4;

	return 0;
}
//------------------------------------------------------------------------------
void popTransaction(int owner)  //deletes all transaction of the user <owner> that has been disconnected
{
	struct transaction *pre, *cur, *next;

	cur = game.transactions_data;

	while (cur && cur == game.transactions_data)
	{
		next = cur->next;

		if (cur->owner == owner)
		{
			free(cur);
			game.transactions_data = next;
		}

		cur = next;
	}

	pre = game.transactions_data;

	while (cur)
	{
		next = cur->next;

		if (cur->owner == owner)
		{
			free(cur);
			pre->next=next;
		}

		cur = next;
	}
}
//------------------------------------------------------------------------------
void performTransactionsRoutines(struct transaction *cur)
{
	int i;
	char *owner_name;

	if (game.planets_data[cur->i * game.height + cur->j].owner != cur->owner)
	{
		for (i = 0; i < game.players; ++i)
			if (game.players_data[i].id == cur->owner)
			{
				*str_broadcast_helper = '\0';
				sprintf(str_broadcast_helper, "%s", game.players_data[i].name);

				break;
			}

		if (cur->number > game.planets_data[cur->i * game.height + cur->j].ships)
		{
			game.planets_data[cur->i * game.height + cur->j].owner = cur->owner;
			game.planets_data[cur->i * game.height + cur->j].ships = cur->number - game.planets_data[cur->i * game.height + cur->j].ships;

			*str_broadcast_adder = '\0';
			sprintf(str_broadcast_adder, "Planet %c has fallen to %s\n", game.planets_data[cur->i * game.height + cur->j].name & 0x7F, str_broadcast_helper);
		}
		else
		{
			game.planets_data[cur->i * game.height + cur->j].ships -= cur->number;

			*str_broadcast_adder = '\0';
			sprintf(str_broadcast_adder, "Planet %c has held against an attack from %s\n", game.planets_data[cur->i * game.height + cur->j].name & 0x7F, str_broadcast_helper);
		}
	}
	else
	{
		game.planets_data[cur->i * game.height + cur->j].ships += cur->number;

		*str_broadcast_adder = '\0';

		for (i = 0; i < game.players; ++i)
			if (game.players_data[i].id == cur->owner)
			{
				owner_name = game.players_data[i].name;

				break;
			}

		sprintf(str_broadcast_adder, "Additional forces (%i ships) of %s came to planet %c\n", cur->number, owner_name, game.planets_data[cur->i * game.height + cur->j].name & 0x7F);
	}

	strcat(str_broadcast, str_broadcast_adder);

	free(cur);
}
//------------------------------------------------------------------------------
void performTransactions()
{
	int i, j;
	struct transaction *pre, *cur, *next;

	for (i = 1; i < game.players; ++i)
		if (game.players_data[i].id)
		{
			pthread_mutex_lock(&game.players_data[i].turn);
			pthread_mutex_unlock(&game.players_data[i].turn);
		}

	for (i = 0; i < game.height; ++i)
		for (j = 0; j < game.width; ++j)
			if (game.planets_data[i * game.height + j].name & 0x80)
				if (game.planets_data[i * game.height + j].owner)
					game.planets_data[i * game.height + j].ships += game.planets_data[i * game.height + j].product;
				else
					game.planets_data[i * game.height + j].ships += game.planets_data[i * game.height + j].product / 4;

	*str_broadcast = '\0';
	sprintf(str_broadcast, "\nEvents of turn #%i:\n", turn++);

	cur = game.transactions_data;

	while (cur && cur == game.transactions_data)
	{
		next = cur->next;

		--cur->delay;

		if (cur->delay <= 0)
		{
			performTransactionsRoutines(cur);
			game.transactions_data = next;
		}

		cur = next;
	}

	pre = game.transactions_data;

	while (cur)
	{
		next = cur->next;

		--cur->delay;

		if (cur->delay <= 0)
		{
			performTransactionsRoutines(cur);
			pre->next=next;
		}

		cur = next;
	}

	pthread_mutex_unlock(&turn_mutex);

	for (i = 1; i < game.players; ++i)
		if (game.players_data[i].id)
		{
			pthread_mutex_lock(&game.players_data[i].sync);
			pthread_mutex_unlock(&game.players_data[i].sync);
		}

	pthread_mutex_lock(&turn_mutex);
}
//------------------------------------------------------------------------------
unsigned char checkLoose(int id)
{
	int my_planets, enemy_planets, my_transactions, enemy_transactions,
		i, j;
	struct transaction *cur;

	my_planets = enemy_planets = my_transactions = enemy_transactions = 0;

	pthread_mutex_lock(&game_mutex);

	for (i = 0; i < game.height; ++i)
		for (j = 0; j < game.width; ++j)
			if (game.planets_data[i * game.height + j].name & 0x80)
				if (game.planets_data[i * game.height + j].owner)
					if (game.planets_data[i * game.height + j].owner == id)
						++my_planets;
					else
						++enemy_planets;

	cur = game.transactions_data;

	while (cur)
	{
		if (cur->owner == id)
			++my_transactions;
		else
			++enemy_transactions;

		cur = cur->next;
	}

	pthread_mutex_unlock(&game_mutex);

	if (enemy_planets + enemy_transactions == 0)
		return YOU_WIN;

	if (my_planets + my_transactions == 0)
			return LOOSE;

	return NEXT_TURN;
}
//------------------------------------------------------------------------------
void waitForOthers(int id)
{
	int i;

	for (i = 1; i < game.players; ++i)
		if (game.players_data[i].id == id)
		{
			pthread_mutex_lock(&game.players_data[i].sync);
			pthread_mutex_unlock(&game.players_data[i].turn);
			pthread_mutex_lock(&turn_mutex);
			pthread_mutex_unlock(&turn_mutex);
			pthread_mutex_unlock(&game.players_data[i].sync);
			pthread_mutex_lock(&game.players_data[i].turn);

			break;
		}
}
//------------------------------------------------------------------------------
void serverThread(int sock)
{
	unsigned char buf[0x1000];
	int no, i;
	char from, to, *sr;
	int num;

	pthread_mutex_lock(&game_mutex);

	for (i = 1; i < game.players; ++i)
		if (game.players_data[i].id == sock)
		{
			pthread_mutex_init(&game.players_data[i].turn, NULL);
			pthread_mutex_lock(&game.players_data[i].turn);
			pthread_mutex_init(&game.players_data[i].sync, NULL);

			break;
		}

	pthread_mutex_unlock(&game_mutex);

	*buf = OK;
	write(sock, buf, 1);

	for (;;)
	{
		no = read(sock, buf, 1);

		if (no <= 0 || EXIT == *buf) break;

		if (MAP == *buf || PLANET == *buf || WHO == *buf)
		{
			pthread_mutex_lock(&game_mutex);

			write(sock, &game, 4 * sizeof(int));  //first 4 fields
			write(sock, game.planets_data, sizeof(struct planet) * game.width * game.height);
			write(sock, game.players_data, sizeof(struct player) * game.players);

			pthread_mutex_unlock(&game_mutex);

			continue;
		}

		if (GO == *buf)
		{
			no = read(sock, buf, 1);

			if (no <= 0) break;

			no = *buf;
			no = read(sock, buf, no);

			if (no <= 0) break;

			sscanf(buf, "%c %c %i", &from, &to, &num);

			*buf = MESSAGE;

			if (!(to = pushTransaction(sock, from, to, num)))
			{
				buf[1] = 1;
				write(sock, buf, 2);
				*buf = '\0';
				write(sock, buf, 1);
			}
			else
			{
				switch (to)
				{
					case 1 :
						sr = str_go_not_exist_destination;

						break;

					case 2 :
						sr = str_go_not_exist_source;

						break;

					case 3 :
						sr = str_go_not_owner;

						break;

					case 4 :
						sr = str_go_not_enought;

						break;
				}

				num = strlen(sr);
				buf[1] = num + 1;
				write(sock, buf, 2);
				memcpy(buf, sr, num + 1);
				write(sock, buf, num + 1);
			}
		}

		if (END == *buf)
		{
			waitForOthers(sock);

			*buf = MESSAGE;
			num = strlen(str_broadcast);
			buf[1] = num + 1;
			write(sock, buf, 2);
			memcpy(buf, str_broadcast, num + 1);
			write(sock, buf, num + 1);

			*buf = checkLoose(sock);
			write(sock, buf, 1);
		}
	}

	shutdown(sock, SHUT_RDWR);

	pthread_mutex_lock(&game_mutex);

	popTransaction(sock);

	for (i = 0; i < game.width * game.height; ++i)
		if (game.planets_data[i].owner == sock)
			game.planets_data[i].owner = 0;

	for (i = 1; i < game.players; ++i)
		if (game.players_data[i].id == sock)
		{
			game.players_data[i].id = 0;
			pthread_mutex_unlock(&game.players_data[i].turn);
			pthread_mutex_destroy(&game.players_data[i].turn);
			pthread_mutex_unlock(&game.players_data[i].sync);
			pthread_mutex_destroy(&game.players_data[i].sync);

			if (no <= 0) fprintf(stderr, "\nUser \"%s\" disconnected\n", game.players_data[i].name);
			else fprintf(stderr, "\nUser \"%s\" exited\n", game.players_data[i].name);

			fflush(stderr);

			break;
		}

	for (i = 1, num = 0; i < game.players; ++i)
		num += game.players_data[i].id;

	if (!num)
	{
		popTransaction(game.players_data[0].id);

		pthread_cancel(server_console);
	}

	pthread_mutex_unlock(&game_mutex);
}
//------------------------------------------------------------------------------
void serverConsoleThread()
{
	int cmd;
	void *op = NULL;

	char from, to, *sr;
	int num;

	pthread_mutex_lock(&turn_mutex);
	pthread_mutex_lock(&game_mutex);
	pthread_mutex_unlock(&game_mutex);

	printf("\nGame started\n");
	printf("\nTurn #%i:\n", turn);

	for (;;op = NULL)
	{
		cmd = console(&op);

		if (EXIT == cmd)
		{
			pthread_mutex_lock(&game_mutex);

			popTransaction(game.players_data[0].id);

			pthread_mutex_unlock(&game_mutex);

			break;
		}

		if (MAP == cmd) dumpMap();

		if (PLANET == cmd) dumpPlanet(*((char *)op));

		if (WHO == cmd) dumpUsers();

		if (GO == cmd)
		{
			sscanf((char *)op, "%c %c %i", &from, &to, &num);

			if (to = pushTransaction(game.players_data[0].id, from, to, num))
			{
				switch (to)
				{
					case 1 :
						sr = str_go_not_exist_destination;

						break;

					case 2 :
						sr = str_go_not_exist_source;

						break;

					case 3 :
						sr = str_go_not_owner;

						break;

					case 4 :
						sr = str_go_not_enought;

						break;
				}

				fputs(sr, stdout);
			}

		}

		if (END == cmd)
		{
			fputs("Waiting for others...\n", stdout);
			fflush(stdout);

			performTransactions();

			fputs(str_broadcast, stdout);

			cmd = checkLoose(game.players_data[0].id);

			if (NEXT_TURN == cmd)
				printf("\nTurn #%i:\n", turn);
			else
			{
				if (LOOSE == cmd)
					fputs("\nYou loose...\n", stdout);
				else
					fputs("\nYou win!\n", stdout);

				break;
			}
		}

		if (op) free(op);
	}
}
//==============================================================================
int serverSide(char *service)
{
	pthread_t threads[MAX_PLAYERS];
	pthread_attr_t ta;
	int sock, socks[MAX_PLAYERS], ssock, connections = 0, no, i;
	struct sockaddr_in fsin;
	unsigned alen;
	unsigned char ans, nas;
	unsigned char name[NAME_LENGTH + 1];

	sock = passivesock(service, 32);  //maximum connections

	initGame();

    printf("Input your nickname: ");
    clearInput();
    fflush(stdout);
    scanf("\n%31s", name);
    strcpy(game.players_data[connections].name, name);
    game.players_data[connections].id = sock;  //sock plays role of id or owner (just different integer numbers - no idea)

    pthread_attr_init(&ta);
    pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_JOINABLE);

    pthread_mutex_lock(&game_mutex);

    for (;;)
    {
        if (connections)
        {
            printf("Start the game or wait for a new connection? (s/w) [w]: ");
            clearInput();
            fflush(stdout);

            ans = getchar();

            if (ans != '\n' && ans != 'w') break;
        }

        printf("Waiting for connections...\n");
        fflush(stdout);

        alen = sizeof(fsin);
        ssock = accept(sock, (struct sockaddr *)&fsin, &alen);

        if (ssock < 0)
        {
            if (EINTR != errno)
                fprintf(stderr, "Cannot accept a new connection : %s\n", strerror(errno));

            continue;
        }

        no = read(ssock, name, NAME_LENGTH + 1);  //read connection signature

        if (0 >= no || SIGNATURE != *name)
        {
            fprintf(stderr, "Connection problem : %s\n", strerror(errno));
            shutdown(ssock, SHUT_RDWR);

            continue;
        }

        name[no] = '\0';

        memmove(name, name + 1, no);

        printf("Incoming connection from \"%s\". Accept? (y/n) [y]: ", name);
        clearInput();
        fflush(stdout);

        ans = getchar();

        if (ans != '\n' && ans != 'y')
        {
            *name = REFUSE;
            write(ssock, name, 1);  //write refuse code
            shutdown(ssock, SHUT_RDWR);

            continue;
        }

        if (pthread_create(&threads[connections], &ta, (void * (*)(void *))serverThread, (void *)ssock))
        {
            fprintf(stderr, "Cannot create a new thread : %s\n", strerror(errno));
            shutdown(ssock, SHUT_RDWR);

            continue;
        }

        socks[connections] = ssock;

        ++connections;

        strcpy(game.players_data[connections].name, name);
        game.players_data[connections].id = ssock;

        if (game.players == connections + 1) break;
    }

    shutdown(sock, SHUT_RDWR);

    game.players = connections + 1;

    initPlanets();

    if (!pthread_create(&server_console, &ta, (void * (*)(void *))serverConsoleThread, NULL))
    {
        pthread_mutex_unlock(&game_mutex);

        pthread_join(server_console, NULL);

        for (i = 0; i < connections; ++i)
        {
            pthread_cancel(threads[i]);
            shutdown(socks[i], SHUT_RDWR);
        }
    }
    else
        fprintf(stderr, "Cannot create a main thread : %s\n", strerror(errno));

    pthread_mutex_destroy(&game_mutex);
    pthread_mutex_destroy(&turn_mutex);

    free(game.planets_data);
    free(game.players_data);

    printf("\nGame is over\n\n");

    return 0;
}
//==============================================================================
int clientSide(char *service, char *host)
{
	unsigned char buf[0x1000];
	int sock, no, cmd, inchars, outchars;
	void *op = NULL;

	sock = connectsock(service, host);

	printf("Input your nickname: ");
	clearInput();
	fflush(stdout);

	scanf("\n%31s", buf);

	printf("Authentication...\n");
	fflush(stdout);

	no = strlen(buf);
	memmove(buf + 1, buf, no);

	*buf = SIGNATURE;
	write(sock, buf, no + 1);

	no = read(sock, buf, 1);

	if (no < 0)
	{
		fprintf(stderr, "Connection problem : %s\n", strerror(errno));
		shutdown(sock, SHUT_RDWR);

		return 1;
	}

	if (REFUSE == *buf || OK !=  *buf)
	{
		fprintf(stderr, "Connection refused\n");
		shutdown(sock, SHUT_RDWR);

		return 0;
	}

	printf("\nGame started\n");

	memset(&game, 0, sizeof(game));
	turn = 1;
	printf("\nTurn #%i:\n", turn++);

	for (;;op = NULL)
	{
		cmd = console(&op);

		if (EXIT == cmd)
		{
			*buf = EXIT;
			write(sock, buf, 1);

			break;
		}

		if (MAP == cmd || PLANET == cmd || WHO == cmd)
		{
			if (game.planets_data) free(game.planets_data);
			if (game.players_data) free(game.players_data);

			game.planets_data = NULL;
			game.players_data = NULL;

			*buf = cmd;
			write(sock, buf, 1);

			outchars = 4 * sizeof(int);

			for (inchars = 0; inchars < outchars; inchars += no)
			{
				no = read(sock, ((char *)&game) + inchars, outchars - inchars);

				if (no <= 0)
				{
					fprintf(stderr, str_server_disconnected);

					goto exit;
				}
			}

			outchars = sizeof(struct planet) * game.width * game.height;

			game.planets_data = (struct planet *)malloc(outchars);

			if (!game.planets_data)
			{
				fprintf(stderr, str_no_memory);

				break;
			}

			for (inchars = 0; inchars < outchars; inchars += no)
			{
				no = read(sock, game.planets_data + inchars, outchars - inchars);

				if (no <= 0)
				{
					fprintf(stderr, str_server_disconnected);

					goto exit;
				}
			}

			outchars = sizeof(struct player) * game.players;

			game.players_data = (struct player *)malloc(outchars);

			if (!game.players_data)
			{
				fprintf(stderr, str_no_memory);

				break;
			}

			for (inchars = 0; inchars < outchars; inchars += no)
			{
				no = read(sock, game.players_data + inchars, outchars - inchars);

				if (no <= 0)
				{
					fprintf(stderr, str_server_disconnected);

					goto exit;
				}
			}

			if (MAP == cmd)
				dumpMap();
			else if (PLANET == cmd)
					dumpPlanet(*(char *)op);
				else
					dumpUsers();
		}

		if (GO == cmd)
		{
			*buf = cmd;
			write(sock, buf, 1);

			*buf = strlen((char *)op) + 1;
			write(sock, buf, 1);
			write(sock, op, *buf);

			no = read(sock, buf, 2);

			if (no <= 0)
			{
				fprintf(stderr, str_server_disconnected);

				break;
			}

			if (*buf != MESSAGE || no != 2)
			{
				fprintf(stderr, str_server_disconnected);

				break;
			}

			outchars = buf[1];

			for (inchars = 0; inchars < outchars; inchars += no)
			{
				no = read(sock, buf + inchars, outchars - inchars);

				if (no <= 0)
				{
					fprintf(stderr, str_server_disconnected);

					goto exit;
				}
			}

			fputs(buf, stdout);
		}

		if (END == cmd)
		{
			*buf = cmd;
			write(sock, buf, 1);

			fputs("Waiting for others...\n", stdout);
			fflush(stdout);

			no = read(sock, buf, 2);

			if (no <= 0)
			{
				fprintf(stderr, str_server_disconnected);

				break;
			}

			if (*buf != MESSAGE || no != 2)
			{
				fprintf(stderr, str_server_disconnected);

				break;
			}

			outchars = buf[1];

			for (inchars = 0; inchars < outchars; inchars += no)
			{
				no = read(sock, buf + inchars, outchars - inchars);

				if (no <= 0)
				{
					fprintf(stderr, str_server_disconnected);

					goto exit;
				}
			}

			fputs(buf, stdout);

			no = read(sock, buf, 1);

			if (no <= 0)
			{
				fprintf(stderr, str_server_disconnected);

				break;
			}

			if (NEXT_TURN == *buf)
				printf("\nTurn #%i:\n", turn++);
			else
			{
				if (LOOSE == *buf)
					fputs("\nYou loose...\n", stdout);
				else
					fputs("\nYou win!\n", stdout);

				break;
			}
		}

		if (op) free(op);
	}

exit:

	shutdown(sock, SHUT_RDWR);

	if (game.planets_data) free(game.planets_data);
	if (game.players_data) free(game.players_data);

	printf("\nGame is over\n\n");

	return 0;
}
//------------------------------------------------------------------------------
