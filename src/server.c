#include "rftg.h"
#include "comm.h"
#include <mysql/mysql.h>
#include <pthread.h>

/*
 * Session status types.
 */
#define SS_EMPTY     0
#define SS_WAITING   1
#define SS_STARTED   2
#define SS_DONE      3
#define SS_ABANDONED 4

/*
 * Number of random bytes to store per session (2 needed per number generated).
 */
#define MAX_RAND     1024


/*
 * A connection from a client.
 */
typedef struct conn
{
	/* File descriptor of socket */
	int fd;

	/* Data buffer for incoming bytes */
	char buf[2048];

	/* Amount of data currently in buffer */
	int buf_full;

	/* Connection state */
	int state;

	/* Username of connected player */
	char user[80];

	/* User ID */
	int uid;

	/* IP address of remote client */
	char addr[80];

	/* Session this player has joined (if any) */
	int sid;

	/* Time of last activity */
	time_t last_active;

	/* Time of last communication */
	time_t last_seen;

} conn;


/*
 * An outstanding choice to be made.
 *
 * We save these in case a client disconnects and later reconnects (and needs
 * to be re-asked), or for when a player is kicked and an AI replacement is
 * added.
 */
typedef struct choice
{
	/* Type */
	int type;

	/* List of choices */
	int list[MAX_DECK];
	int num;

	/* Special options */
	int special[MAX_DECK];
	int num_special;

	/* Arguments */
	int arg1;
	int arg2;
	int arg3;

} choice;

/*
 * A game to be started, or in progress.
 */
typedef struct session
{
	/* Session description */
	char desc[1024];

	/* Password needed to join this game */
	char pass[21];

	/* Game number */
	int gid;

	/* Game information */
	game g;

	/* Game information remembered by each client */
	game old[MAX_PLAYER];

	/* Outstanding choice for each player */
	choice out[MAX_PLAYER];

	/* Pool of random bytes */
	unsigned char random_pool[MAX_RAND];

	/* Current position in random pool */
	int random_pos;

	/* User ID who created session */
	int created;

	/* List of users who have joined this session */
	int uids[MAX_PLAYER];

	/* Connection IDs of users (if online) */
	int cids[MAX_PLAYER];

	/* Player has been taken over by an AI */
	int ai_control[MAX_PLAYER];

	/* Number of users attached to this session */
	int num_users;

	/* Session state */
	int state;

	/* Desired min/max number of players */
	int min_player;
	int max_player;

	/* Desired expansion level */
	int expanded;

	/* Other options */
	int advanced;
	int disable_goal;
	int disable_takeover;

	/* Game speed */
	int speed;

	/* Player(s) being waited upon */
	int waiting[MAX_PLAYER];

	/* Ticks (roughly 10 seconds) we've been waiting on each player */
	int wait_ticks[MAX_PLAYER];

	/* Mutex for access to waiting flags */
	pthread_mutex_t wait_mutex;

	/* Condition variable to signal game thread when reply is ready */
	pthread_cond_t wait_cond;

} session;


/*
 * List of all active connections.
 */
static conn c_list[1024];
static int num_conn;

/*
 * List of active game sessions.
 */
static session s_list[1024];
static int num_session;

/*
 * Mutex preventing more than one game from calling the AI at once.
 */
static pthread_mutex_t ai_mutex;


/*
 * Connection to the database server.
 */
MYSQL *mysql;

/*
 * Check for a user in the database with the given password.
 *
 * If the user does not exist, create an entry for them.
 *
 * Return -1 if the password given does not match an existing entry.
 */
static int db_user(char *user, char *pass)
{
	MYSQL_RES *res;
	MYSQL_ROW row;
	char query[1024];
	char euser[1024], epass[1024];
	int uid;

	/* Escape user and password */
	mysql_real_escape_string(mysql, euser, user, strlen(user));
	mysql_real_escape_string(mysql, epass, pass, strlen(pass));

	/* Create lookup query */
	sprintf(query, "SELECT pass, uid FROM users WHERE user='%s'", euser);

	/* Run query */
	mysql_query(mysql, query);

	/* Fetch results */
	res = mysql_store_result(mysql);

	/* Check for no rows returned */
	if (!(row = mysql_fetch_row(res)))
	{
		/* Free old results */
		mysql_free_result(res);

		/* Create insertion query */
		sprintf(query, "INSERT INTO users (user, pass) VALUES \
		        ('%s', '%s')", euser, epass);

		/* Send query */
		mysql_query(mysql, query);

		/* Get ID of user inserted */
		strcpy(query, "SELECT LAST_INSERT_ID()");

		/* Run query */
		mysql_query(mysql, query);

		/* Fetch results */
		res = mysql_store_result(mysql);

		/* Get row */
		row = mysql_fetch_row(res);

		/* Get user ID */
		uid = strtol(row[0], NULL, 0);

		/* Free result */
		mysql_free_result(res);

		/* Return ID */
		return uid;
	}

	/* Check for matching password */
	if (!strcmp(pass, row[0]))
	{
		/* Get ID */
		uid = strtol(row[1], NULL, 0);

		/* Free result */
		mysql_free_result(res);

		/* Return ID */
		return uid;
	}

	/* Free result */
	mysql_free_result(res);

	/* Bad password */
	return -1;
}

/*
 * Return the user name for a given user ID.
 */
static void db_user_name(int uid, char *name)
{
	MYSQL_RES *res;
	MYSQL_ROW row;
	char query[1024];

	/* Create query */
	sprintf(query, "SELECT user FROM users WHERE uid=%d", uid);

	/* Run query */
	mysql_query(mysql, query);

	/* Fetch results */
	res = mysql_store_result(mysql);

	/* Get row */
	row = mysql_fetch_row(res);

	/* Copy user name */
	strcpy(name, row[0]);

	/* Free result */
	mysql_free_result(res);
}

/*
 * Create an entry for a game in the database.
 *
 * Return the game ID.
 */
static int db_new_game(int sid)
{
	MYSQL_RES *res;
	MYSQL_ROW row;
	session *s_ptr = &s_list[sid];
	char query[1024];
	char edesc[1024], epass[1024];
	int gid;

	/* Escape game description and password */
	mysql_real_escape_string(mysql, edesc, s_ptr->desc,strlen(s_ptr->desc));
	mysql_real_escape_string(mysql, epass, s_ptr->pass,strlen(s_ptr->pass));

	/* Create insertion query */
	sprintf(query, "INSERT INTO games (description, pass, created, state, \
	                                   minp, maxp, \
	                                   exp, adv, dis_goal, dis_takeover, \
	                                   speed) \
	             VALUES ('%s', '%s', %d, 'WAITING', %d, %d, %d, %d, \
	                     %d, %d, %d)",
	        edesc, epass, s_ptr->created,
	        s_ptr->min_player, s_ptr->max_player,
	        s_ptr->expanded, s_ptr->advanced, s_ptr->disable_goal,
	        s_ptr->disable_takeover, s_ptr->speed);

	/* Send query */
	mysql_query(mysql, query);

	/* Check for error */
	if (*mysql_error(mysql))
	{
		/* Print error */
		printf("%s\n", mysql_error(mysql));
		exit(1);
	}

	/* Get ID of game inserted */
	strcpy(query, "SELECT LAST_INSERT_ID()");

	/* Run query */
	mysql_query(mysql, query);

	/* Fetch results */
	res = mysql_store_result(mysql);

	/* Get row */
	row = mysql_fetch_row(res);

	/* Get ID */
	gid = strtol(row[0], NULL, 0);

	/* Free result */
	mysql_free_result(res);

	/* Return ID */
	return gid;
}

/*
 * Read waiting/running games from database.
 */
static void db_load_sessions(void)
{
	MYSQL_RES *res;
	MYSQL_ROW row;
	session *s_ptr;
	int sid = 0;

	/* Run query */
	mysql_query(mysql, "SELECT gid, description, pass, created, state, \
	                           minp, maxp, exp, adv, dis_goal, \
	                           dis_takeover, speed \
	                    FROM games \
	                    WHERE state='WAITING' OR state='STARTED'");

	/* Fetch results */
	res = mysql_store_result(mysql);

	/* Loop over rows returned */
	while ((row = mysql_fetch_row(res)))
	{
		/* Get pointer to session */
		s_ptr = &s_list[sid];

		/* Read fields */
		s_ptr->gid = strtol(row[0], NULL, 0);
		strcpy(s_ptr->desc, row[1]);
		strcpy(s_ptr->pass, row[2]);
		s_ptr->created = strtol(row[3], NULL, 0);

		/* Check state */
		if (!strcmp(row[4], "WAITING"))
		{
			/* Set state */
			s_ptr->state = SS_WAITING;
		}
		else
		{
			/* Game is started */
			s_ptr->state = SS_STARTED;
		}

		/* Read fields */
		s_ptr->min_player = strtol(row[5], NULL, 0);
		s_ptr->max_player = strtol(row[6], NULL, 0);
		s_ptr->expanded = strtol(row[7], NULL, 0);
		s_ptr->advanced = strtol(row[8], NULL, 0);
		s_ptr->disable_goal = strtol(row[9], NULL, 0);
		s_ptr->disable_takeover = strtol(row[10], NULL, 0);
		s_ptr->speed = strtol(row[11], NULL, 0);

		/* Increase number of sessions */
		num_session++;

		/* Go to next session ID */
		sid++;
	}

	/* Free results */
	mysql_free_result(res);
}

/*
 * Read list of players in waiting/running games from database.
 */
static void db_load_attendance(void)
{
	MYSQL_RES *res;
	MYSQL_ROW row;
	session *s_ptr;
	int uid, gid, ai;
	int i;

	/* Run query */
	mysql_query(mysql, "SELECT uid, gid, ai \
	                    FROM attendance \
	                    JOIN games USING (gid) \
	                    WHERE state='WAITING' OR state='STARTED' \
	                    ORDER BY seat");

	/* Fetch results */
	res = mysql_store_result(mysql);

	/* Loop over rows returned */
	while ((row = mysql_fetch_row(res)))
	{
		/* Get user ID */
		uid = strtol(row[0], NULL, 0);

		/* Get game ID */
		gid = strtol(row[1], NULL, 0);

		/* Get AI control */
		ai = strtol(row[2], NULL, 0);

		/* Loop over sessions */
		for (i = 0; i < num_session; i++)
		{
			/* Stop at correct game ID */
			if (s_list[i].gid == gid) break;
		}

		/* Check for error */
		if (i == num_session)
		{
			/* Error */
			printf("Bad attendance: no gid %d\n", gid);
			continue;
		}

		/* Get pointer to session */
		s_ptr = &s_list[i];

		/* Add user to session */
		s_ptr->uids[s_ptr->num_users] = uid;

		/* No connection for user yet */
		s_ptr->cids[s_ptr->num_users] = -1;

		/* Set AI control */
		s_ptr->ai_control[s_ptr->num_users] = ai;

		/* Count users */
		s_ptr->num_users++;
	}

	/* Free results */
	mysql_free_result(res);
}

/*
 * Add a player to a game in the database.
 */
static void db_join_game(int uid, int gid)
{
	char query[1024];

	/* Create query */
	sprintf(query, "INSERT INTO attendance (uid, gid) VALUES (%d, %d)",
	        uid, gid);

	/* Run query */
	mysql_query(mysql, query);
}

/*
 * Remove a player from a game in the database.
 */
static void db_leave_game(int uid, int gid)
{
	char query[1024];

	/* Create query */
	sprintf(query, "DELETE FROM attendance WHERE uid=%d AND gid=%d",
	        uid, gid);

	/* Run query */
	mysql_query(mysql, query);
}

/*
 * Load a game's saved state from the database.
 *
 * Return 0 if no state is available.
 */
static int db_load_game_state(int sid)
{
	MYSQL_RES *res;
	MYSQL_ROW row;
	session *s_ptr = &s_list[sid];
	unsigned long *field_len;
	char query[1024];
	int i;

	/* Create query */
	sprintf(query, "SELECT pool FROM seed WHERE gid=%d", s_ptr->gid);

	/* Run query */
	mysql_query(mysql, query);

	/* Fetch results */
	res = mysql_store_result(mysql);

	/* Check for no rows returned */
	if (!(row = mysql_fetch_row(res)))
	{
		/* Free result */
		mysql_free_result(res);

		/* No pool to load */
		return 0;
	}

	/* Copy returned data to random byte pool */
	memcpy(s_ptr->random_pool, row[0], MAX_RAND);

	/* Start at beginning of byte pool */
	s_ptr->random_pos = 0;

	/* Free result */
	mysql_free_result(res);

	/* Loop over players in session */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Create query to load choice log */
		sprintf(query,"SELECT log FROM choices WHERE gid=%d AND uid=%d",
		        s_ptr->gid, s_ptr->uids[i]);

		/* Run query */
		mysql_query(mysql, query);

		/* Fetch results */
		res = mysql_store_result(mysql);

		/* Check for no rows returned */
		if (!(row = mysql_fetch_row(res)))
		{
			/* Free result */
			mysql_free_result(res);

			/* Go to next player */
			continue;
		}

		/* Get length of log in bytes */
		field_len = mysql_fetch_lengths(res);

		/* Copy log */
		memcpy(s_ptr->g.p[i].choice_log, row[0], field_len[0]);

		/* Remember length */
		s_ptr->g.p[i].choice_size = field_len[0] / sizeof(int);

		/* Free result */
		mysql_free_result(res);
	}

	/* Success */
	return 1;
}

/*
 * Save the basic state about a game, including random seeds and which
 * players begin in each seat.
 */
static void db_save_game_state(int sid)
{
	session *s_ptr = &s_list[sid];
	char query[4096], pool[4096], *status = "";

	/* Determine session status */
	switch (s_ptr->state)
	{
		case SS_WAITING: status = "WAITING"; break;
		case SS_STARTED: status = "STARTED"; break;
		case SS_DONE: status = "DONE"; break;
		case SS_ABANDONED: status = "ABANDONED"; break;
	}

	/* Create query for session status */
	sprintf(query, "UPDATE games SET state='%s' WHERE gid=%d", status,
	        s_ptr->gid);

	/* Run query */
	mysql_query(mysql, query);

	/* No need to save further data if game has not started */
	if (s_ptr->state == SS_WAITING || s_ptr->state == SS_ABANDONED) return;

	/* Escape random byte pool */
	mysql_real_escape_string(mysql, pool, (char *)s_ptr->random_pool,
	                         MAX_RAND);

	/* Create query to save random byte pool */
	sprintf(query, "INSERT IGNORE INTO seed VALUES (%d, '%s')",
	        s_ptr->gid, pool);

	/* Run query */
	mysql_query(mysql, query);
}

/*
 * Save the initial seating position of players just before a game starts.
 */
static void db_save_seats(int sid)
{
	session *s_ptr = &s_list[sid];
	char query[1024];
	int i;

	/* Loop over players in game */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Update seat number */
		sprintf(query, "UPDATE attendance SET seat=%d, ai=%d \
		                WHERE gid=%d AND uid=%d",
		                i, s_ptr->ai_control[i],
		                s_ptr->gid, s_ptr->uids[i]);

		/* Run query */
		mysql_query(mysql, query);
	}
}

/*
 * Save a player's choice log to the database.
 */
static void db_save_choices(int sid, int who)
{
	session *s_ptr = &s_list[sid];
	player *p_ptr;
	char query[20000], log[20000];

	/* Get player pointer */
	p_ptr = &s_ptr->g.p[who];

	/* Escape choice log string */
	mysql_real_escape_string(mysql, log, (char *)p_ptr->choice_log,
	                         sizeof(int) * p_ptr->choice_size);

	/* Create query */
	sprintf(query, "REPLACE INTO choices VALUES (%d, %d, '%s')", s_ptr->gid,
	        s_ptr->uids[who], log);

	/* Run query */
	mysql_query(mysql, query);
}

/*
 * Send information about a player to all clients.
 */
static void send_player_one(int dest, int who)
{
	/* Check for disconnected player */
	if (c_list[who].state == CS_DISCONN)
	{
		/* Send "player left" message */
		send_msgf(c_list[dest].fd, MSG_PLAYER_LEFT, "s",
		          c_list[who].user);
	}
	else
	{
		/* Send "new player" message */
		send_msgf(c_list[dest].fd, MSG_PLAYER_NEW, "s",
		          c_list[who].user);
	}
}

/*
 * Send information about a player to all connected clients.
 */
static void send_player(int who)
{
	int i;

	/* Loop over connections */
	for (i = 0; i < num_conn; i++)
	{
		/* Skip non-active connections */
		if (c_list[i].state != CS_LOBBY &&
		    c_list[i].state != CS_PLAYING) continue;

		/* Send information */
		send_player_one(i, who);
	}
}

/*
 * Send information about all connected players to a new client.
 */
static void send_all_players(int dest)
{
	int i;

	/* Loop over connections */
	for (i = 0; i < num_conn; i++)
	{
		/* Skip non-active connections */
		if (c_list[i].state != CS_LOBBY &&
		    c_list[i].state != CS_PLAYING) continue;

		/* Send to player */
		send_player_one(dest, i);
	}
}

/*
 * Send information about an open game to a client.
 */
static void send_session_one(int sid, int cid)
{
	session *s_ptr = &s_list[sid];
	char name[1024];
	int i;

	/* Check for game not in waiting status */
	if (s_ptr->state != SS_WAITING)
	{
		/* Tell client that game is closed */
		send_msgf(c_list[cid].fd, MSG_CLOSE_GAME, "d", sid);

		/* Done */
		return;
	}

	/* Get username of game creator */
	db_user_name(s_ptr->created, name);

	/* Send message to client */
	send_msgf(c_list[cid].fd, MSG_OPENGAME, "dssddddddddd",
	          sid, s_ptr->desc, name, strlen(s_ptr->pass) > 0,
	          s_ptr->min_player, s_ptr->max_player,
	          s_ptr->expanded, s_ptr->advanced, s_ptr->disable_goal,
	          s_ptr->disable_takeover, s_ptr->speed,
	          c_list[cid].uid == s_ptr->created);

	/* Loop over player spots */
	for (i = 0; i < MAX_PLAYER; i++)
	{
		/* Check for empty player */
		if (i >= s_ptr->num_users)
		{
			/* Send empty player spot */
			send_msgf(c_list[cid].fd, MSG_GAME_PLAYER, "ddsdd",
			          sid, i, "", 0, 0);

			/* Next spot */
			continue;
		}

		/* Get user name for player */
		db_user_name(s_ptr->uids[i], name);

		/* Send message about joined player */
		send_msgf(c_list[cid].fd, MSG_GAME_PLAYER, "ddsdd",
		          sid, i, name, s_ptr->cids[i] != -1,
		          s_ptr->cids[i] == cid);
	}
}

/*
 * Send information about a session to every connected player.
 */
static void send_session(int sid)
{
	int cid;

	/* Loop over connections */
	for (cid = 0; cid < num_conn; cid++)
	{
		/* Skip non-active connections */
		if (c_list[cid].state != CS_LOBBY &&
		    c_list[cid].state != CS_PLAYING) continue;

		/* Send game state */
		send_session_one(sid, cid);
	}
}

/*
 * Send information about all open sessions to a client.
 */
static void send_open_sessions(int cid)
{
	int sid;

	/* Loop over sessions */
	for (sid = 0; sid < num_session; sid++)
	{
		/* Check for game waiting for players */
		if (s_list[sid].state == SS_WAITING)
		{
			/* Send game state */
			send_session_one(sid, cid);
		}
	}
}

/*
 * Send a message to all connected clients of a session.
 */
static void send_to_session(int sid, char *msg)
{
	session *s_ptr = &s_list[sid];
	int i, cid;

	/* Loop over users in a session */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Get connection ID of this user */
		cid = s_ptr->cids[i];

		/* Check for no connection */
		if (cid < 0) continue;

		/* Send to client */
		send_msg(c_list[cid].fd, msg);
	}
}

/*
 * Handle a game message.
 */
void message_add(game *g, char *txt)
{
	char msg[1024], *ptr = msg;

	/* Create log message */
	start_msg(&ptr, MSG_LOG);

	/* Add text of message */
	put_string(txt, &ptr);

	/* Finish message */
	finish_msg(msg, ptr);

	/* Send message to all clients in game */
	send_to_session(g->session_id, msg);
}

/*
 * Wait for player to have an answer ready.
 */
void wait_for_answer(game *g, int who)
{
	session *s_ptr;

	/* Do not wait for in simulated game */
	if (g->simulation) return;

	/* Get session pointer */
	s_ptr = &s_list[g->session_id];

	/* Acquire wait mutex */
	pthread_mutex_lock(&s_ptr->wait_mutex);

	/* Wait until player is ready */
	while (s_ptr->waiting[who])
	{
		/* Wait for signal */
		pthread_cond_wait(&s_ptr->wait_cond, &s_ptr->wait_mutex);
	}

	/* Unlock wait mutex */
	pthread_mutex_unlock(&s_ptr->wait_mutex);
}

/*
 * Initialize random byte pool for a session.
 *
 * We use the kernel's /dev/urandom service to provide the bytes.
 */
static void init_random_pool(int sid)
{
	session *s_ptr = &s_list[sid];
	int fd;
	int x, total = 0;

	/* Open random device */
	fd = open("/dev/urandom", O_RDONLY);

	/* Check for error */
	if (fd < 0)
	{
		/* Print error and exit */
		perror("/dev/urandom");
		exit(1);
	}

	/* Read bytes */
	while (total < MAX_RAND)
	{
		/* Read as many bytes as needed */
		x = read(fd, s_ptr->random_pool + total, MAX_RAND - total);

		/* Check for error */
		if (x < 0)
		{
			/* Print error and exit */
			perror("read");
			exit(1);
		}

		/* Add to total read */
		total += x;
	}

	/* Close random device */
	close(fd);

	/* Start at beginning of pool */
	s_ptr->random_pos = 0;
}

/*
 * More complex random number generator for multiplayer games.
 *
 * Call simple RNG in simulated games, otherwise use the Mersenne Twister
 * algorithm with a random seed saved per session.
 */
int game_rand(game *g)
{
	session *s_ptr = &s_list[g->session_id];
	unsigned int x;

	/* Check for simulated game */
	if (g->simulation)
	{
		/* Use simple random number generator */
		return simple_rand(&g->random_seed);
	}

	/* Check for end of random bytes reached */
	if (s_ptr->random_pos == MAX_RAND)
	{
		/* XXX Restart from beginning */
		s_ptr->random_pos = 0;
	}

	/* Create random number from next two bytes */
	x = s_ptr->random_pool[s_ptr->random_pos++];
	x |= s_ptr->random_pool[s_ptr->random_pos++] << 8;

	/* Return low bits */
	return x & 0x7fff;
}

/*
 * Tell all clients about game parameters.
 */
static void update_meta(int sid)
{
	session *s_ptr = &s_list[sid];
	char msg[1024], *ptr = msg;
	int i;

	/* Start message */
	start_msg(&ptr, MSG_STATUS_META);

	/* Add game parameters to message */
	put_integer(s_ptr->num_users, &ptr);
	put_integer(s_ptr->expanded, &ptr);
	put_integer(s_ptr->advanced, &ptr);
	put_integer(s_ptr->disable_goal, &ptr);
	put_integer(s_ptr->disable_takeover, &ptr);

	/* Loop over goals */
	for (i = 0; i < MAX_GOAL; i++)
	{
		/* Add goal presence to message */
		put_integer(s_ptr->g.goal_active[i], &ptr);
	}

	/* Loop over players */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Add player's name to message */
		put_string(s_ptr->g.p[i].name, &ptr);
	}

	/* Finish message */
	finish_msg(msg, ptr);

	/* Send to everyone */
	send_to_session(sid, msg);

	/* Loop over players */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Clear old game structure */
		memset(&s_ptr->old[i], 0, sizeof(game));
	}
}

/*
 * Obfuscate information about cards that the given player should not know.
 */
static void obfuscate_game(game *ob, game *g, int who)
{
	int i, j;

	/* Copy game state */
	*ob = *g;

	/* Loop over cards */
	for (i = 0; i < g->deck_size; i++)
	{
		/* Check for active card (known to all) */
		if (g->deck[i].where == WHERE_ACTIVE) continue;

		/* Check for card owned by player (but not a good) */
		if (g->deck[i].owner == who && g->deck[i].where != WHERE_GOOD)
			continue;

		/* Clear card location */
		ob->deck[i].owner = -1;
		ob->deck[i].where = WHERE_DECK;
		ob->deck[i].temp = 0;
	}

	/* Start at beginning of obfuscated deck */
	j = 0;

	/* Loop over cards */
	for (i = 0; i < g->deck_size; i++)
	{
		/* Skip cards in draw pile */
		if (g->deck[i].where == WHERE_DECK) continue;

		/* Skip active cards */
		if (g->deck[i].where == WHERE_ACTIVE) continue;

		/* Skip cards known by owner */
		if (g->deck[i].owner == who && g->deck[i].where != WHERE_GOOD)
			continue;

		/* Find substitute card */
		while (ob->deck[j].where != WHERE_DECK) j++;

		/* Copy card location */
		ob->deck[j].where = g->deck[i].where;
		ob->deck[j].owner = g->deck[i].owner;
	}

	/* Start at beginning of deck */
	j = 0;

	/* Loop over cards */
	for (i = 0; i < g->deck_size; i++)
	{
		/* Skip cards without a good */
		if (ob->deck[i].covered < 0) continue;

		/* Find substitute good */
		while (ob->deck[j].where != WHERE_GOOD) j++;

		/* Use this card for a good */
		ob->deck[i].covered = j;

		/* Go to next card in deck */
		j++;
	}
}

/*
 * Return true if two player structures differ in a way that requires
 * resending to a client.
 */
static int player_changed(player *p_ptr, player *q_ptr)
{
	int i;

	/* Check for change in actions selected */
	if (p_ptr->action[0] != q_ptr->action[0]) return 1;
	if (p_ptr->action[1] != q_ptr->action[1]) return 1;

	/* Check for change in VP/prestige/etc */
	if (p_ptr->vp != q_ptr->vp) return 1;
	if (p_ptr->prestige != q_ptr->prestige) return 1;
	if (p_ptr->prestige_action_used != q_ptr->prestige_action_used)return 1;

	/* Loop over goals */
	for (i = 0; i < MAX_GOAL; i++)
	{
		/* Check for change in goal parameters */
		if (p_ptr->goal_claimed[i] != q_ptr->goal_claimed[i]) return 1;
		if (p_ptr->goal_progress[i] != q_ptr->goal_progress[i])return 1;
	}

	/* No change */
	return 0;
}

/*
 * Send updates to game status to one client.
 */
static void update_status_one(int sid, int who)
{
	session *s_ptr = &s_list[sid];
	game obfus;
	player *p_ptr;
	card *c_ptr;
	char msg[1024], *ptr;
	int i, j;

	/* Obfuscate hidden information for this player */
	obfuscate_game(&obfus, &s_ptr->g, who);

	/* Check for change in player status */
	for (i = 0; i < s_ptr->g.num_players; i++)
	{
		/* Check for difference in status */
		if (player_changed(&obfus.p[i], &s_ptr->old[who].p[i]))
		{
			/* Get player pointer */
			p_ptr = &obfus.p[i];

			/* Start at beginning of message buffer */
			ptr = msg;

			/* Start message about player */
			start_msg(&ptr, MSG_STATUS_PLAYER);

			/* Add player number to message */
			put_integer(i, &ptr);

			/* Check for whether to send actions */
			if (obfus.cur_action >= ACT_SEARCH ||
			    count_active_flags(&obfus, who, FLAG_SELECT_LAST))
			{
				/* Add actions to message */
				put_integer(p_ptr->action[0], &ptr);
				put_integer(p_ptr->action[1], &ptr);
			}
			else
			{
				/* Add empty actions to message */
				put_integer(-1, &ptr);
				put_integer(-1, &ptr);
			}

			/* Add prestige action/search used flag */
			put_integer(p_ptr->prestige_action_used, &ptr);

			/* Loop over goals */
			for (j = 0; j < MAX_GOAL; j++)
			{
				/* Add whether player has claimed goal */
				put_integer(p_ptr->goal_claimed[j], &ptr);

				/* Add player's progress toward goal */
				put_integer(p_ptr->goal_progress[j], &ptr);
			}

			/* Add player's prestige count */
			put_integer(p_ptr->prestige, &ptr);

			/* Add player's VP count */
			put_integer(p_ptr->vp, &ptr);

			/* Finish message */
			finish_msg(msg, ptr);

			/* Send to client */
			send_msg(c_list[s_ptr->cids[who]].fd, msg);
		}
	}

	/* Loop over cards in deck */
	for (i = 0; i < obfus.deck_size; i++)
	{
		/* Check for difference from before */
		if (memcmp(&obfus.deck[i], &s_ptr->old[who].deck[i],
		           sizeof(card)))
		{
			/* Get card pointer */
			c_ptr = &obfus.deck[i];

			/* Start at beginning of message buffer */
			ptr = msg;

			/* Start message about card */
			start_msg(&ptr, MSG_STATUS_CARD);

			/* Add card index */
			put_integer(i, &ptr);

			/* Add card owner */
			put_integer(c_ptr->owner, &ptr);

			/* Add card location */
			put_integer(c_ptr->where, &ptr);

			/* Add temporary flag */
			put_integer(c_ptr->temp, &ptr);

			/* Add order played on table */
			put_integer(c_ptr->order, &ptr);

			/* Add covered flag */
			put_integer(c_ptr->covered, &ptr);

			/* Finish message */
			finish_msg(msg, ptr);

			/* Send to client */
			send_msg(c_list[s_ptr->cids[who]].fd, msg);
		}
	}

	/* Check for change in goal status */
	if (memcmp(obfus.goal_avail, s_ptr->old[who].goal_avail,
	           MAX_GOAL * sizeof(int)) ||
	    memcmp(obfus.goal_most, s_ptr->old[who].goal_most,
	           MAX_GOAL * sizeof(int)))
	{
		/* Start at beginning of message buffer */
		ptr = msg;

		/* Start message about goals */
		start_msg(&ptr, MSG_STATUS_GOAL);

		/* Copy goal availability and most progress */
		for (i = 0; i < MAX_GOAL; i++)
		{
			/* Put availabiltiy and progress counts */
			put_integer(s_ptr->g.goal_avail[i], &ptr);
			put_integer(s_ptr->g.goal_most[i], &ptr);
		}

		/* Finish message */
		finish_msg(msg, ptr);

		/* Send to client */
		send_msg(c_list[s_ptr->cids[who]].fd, msg);
	}

	/* Copy game state to remembered */
	s_ptr->old[who] = obfus;
}

/*
 * Send updates to game status to all clients in a session.
 */
static void update_status(int sid)
{
	session *s_ptr = &s_list[sid];
	char msg[1024], *ptr;
	int i;

	/* Send individualized status to everyone */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Skip players who are not connected */
		if (s_ptr->cids[i] < 0) continue;

		/* Send updates */
		update_status_one(sid, i);
	}

	/* Start at beginning of message buffer */
	ptr = msg;

	/* Create miscellaneous status update message */
	start_msg(&ptr, MSG_STATUS_MISC);

	/* Add round number to message */
	put_integer(s_ptr->g.round, &ptr);

	/* Add size of VP pool to message */
	put_integer(s_ptr->g.vp_pool, &ptr);

	/* Loop over actions */
	for (i = 0; i < MAX_ACTION; i++)
	{
		/* Add flag for action selected */
		put_integer(s_ptr->g.action_selected[i], &ptr);
	}

	/* Add current action to message */
	put_integer(s_ptr->g.cur_action, &ptr);

	/* Finish message */
	finish_msg(msg, ptr);

	/* Send to session */
	send_to_session(sid, msg);
}

/*
 * Send waiting messages for players in session.
 */
static void update_waiting(int sid)
{
	session *s_ptr = &s_list[sid];
	char msg[1024], *ptr = msg;
	int i;

	/* Start waiting message */
	start_msg(&ptr, MSG_WAITING);

	/* Lock waiting mutex */
	pthread_mutex_lock(&s_ptr->wait_mutex);

	/* Loop over players */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Add waiting status to message */
		put_integer(s_ptr->waiting[i], &ptr);
	}

	/* Unlock mutex */
	pthread_mutex_unlock(&s_ptr->wait_mutex);

	/* Finish message */
	finish_msg(msg, ptr);

	/* Send to everyone */
	send_to_session(sid, msg);
}

/*
 * Player spots have been rotated.
 */
static void server_notify_rotation(game *g, int who)
{
	session *s_ptr = &s_list[g->session_id];
	int temp_uid, temp_cid, temp_ai;
	int i;

	/* XXX Only do this once per set of players */
	if (who != 0) return;

	/* Copy player 0 information */
	temp_uid = s_ptr->uids[0];
	temp_cid = s_ptr->cids[0];
	temp_ai = s_ptr->ai_control[0];

	/* Loop over players */
	for (i = 0; i < s_ptr->num_users - 1; i++)
	{
		/* Move info one space */
		s_ptr->uids[i] = s_ptr->uids[i + 1];
		s_ptr->cids[i] = s_ptr->cids[i + 1];
		s_ptr->ai_control[i] = s_ptr->ai_control[i + 1];
	}

	/* Store old player 0 info in last spot */
	s_ptr->uids[i] = temp_uid;
	s_ptr->cids[i] = temp_cid;
	s_ptr->ai_control[i] = temp_ai;

	/* Loop over players */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Skip players who are not connected */
		if (s_ptr->cids[i] < 0) continue;

		/* Tell player about new seat */
		send_msgf(c_list[s_ptr->cids[i]].fd, MSG_SEAT, "d", i);
	}

	/* Tell players about new names in seats */
	update_meta(g->session_id);
}

/*
 * Ask player to prepare choices for the given phase.
 */
void prepare_phase(game *g, int who, int phase, int arg)
{
	session *s_ptr = &s_list[g->session_id];

	/* Don't prepare in simulated games */
	if (g->simulation) return;

	/* Don't ask players who aren't connected */
	if (s_ptr->cids[who] < 0) return;

	/* Don't ask players who already have choices in log */
	if (g->p[who].choice_size > g->p[who].choice_pos) return;

	/* Send game updates to session */
	update_status(g->session_id);

	/* Ask player to prepare */
	send_msgf(c_list[s_ptr->cids[who]].fd, MSG_PREPARE, "ddd",
	          g->p[who].choice_size, phase, arg);

	/* Lock waiting mutex */
	pthread_mutex_lock(&s_ptr->wait_mutex);

	/* Player has option to play */
	s_ptr->waiting[who] = WAIT_OPTION;

	/* Unlock mutex */
	pthread_mutex_unlock(&s_ptr->wait_mutex);

	/* printf("Asking %d to prepare for phase %d at %d\n", s_ptr->cids[who], phase, g->p[who].choice_size); */
}

/*
 * (Re-)Ask a client to make a game choice.
 */
static void ask_client(int sid, int who)
{
	session *s_ptr = &s_list[sid];
	conn *c_ptr;
	choice *o_ptr;
	int cid;
	char msg[1024], *ptr = msg;
	int i;

	/* Send game updates to players */
	update_status(sid);

	/* Get choice pointer */
	o_ptr = &s_ptr->out[who];

	/* Get connection ID for this player */
	cid = s_ptr->cids[who];

	/* Lock waiting mutex */
	pthread_mutex_lock(&s_ptr->wait_mutex);

	/* Check for no outstanding choice for this client */
	if (s_ptr->waiting[who] == WAIT_READY ||
	    s_ptr->waiting[who] == WAIT_OPTION)
	{
		/* Unlock mutex */
		pthread_mutex_unlock(&s_ptr->wait_mutex);

		/* Nothing to do */
		return;
	}

	/* Unlock mutex */
	pthread_mutex_unlock(&s_ptr->wait_mutex);

	/* Check for AI control */
	if (s_ptr->ai_control[who])
	{
		/* Grab AI mutex */
		pthread_mutex_lock(&ai_mutex);

		/* Initialize AI neural nets for this game */
		ai_func.init(&s_ptr->g, who, 0);

		/* Ask AI to make choice */
		ai_func.make_choice(&s_ptr->g, who, o_ptr->type,
		                    o_ptr->list, &o_ptr->num,
		                    o_ptr->special, &o_ptr->num_special,
		                    o_ptr->arg1, o_ptr->arg2, o_ptr->arg3);

		/* Release AI mutex */
		pthread_mutex_unlock(&ai_mutex);

		/* Acquire wait mutex for session */
		pthread_mutex_lock(&s_ptr->wait_mutex);

		/* Mark player as no longer waiting */
		s_ptr->waiting[who] = WAIT_READY;

		/* Signal game thread to continue */
		pthread_cond_signal(&s_ptr->wait_cond);

		/* Unlock wait mutex */
		pthread_mutex_unlock(&s_ptr->wait_mutex);

		/* Update waiting status */
		update_waiting(sid);

		/* Done */
		return;
	}

	/* Check for no player */
	if (cid < 0) return;

	/* Get connection pointer */
	c_ptr = &c_list[cid];

	/* printf("Asking %d for choice (type %d) at %d\n", cid, o_ptr->type, s_ptr->g.p[who].choice_size); */

	/* Start choice message */
	start_msg(&ptr, MSG_CHOOSE);

	/* Add current choice log position */
	put_integer(s_ptr->g.p[who].choice_size, &ptr);

	/* Add choice type */
	put_integer(o_ptr->type, &ptr);

	/* Add number of items in list */
	put_integer(o_ptr->num, &ptr);

	/* Add each item in list */
	for (i = 0; i < o_ptr->num; i++)
	{
		/* Add item */
		put_integer(o_ptr->list[i], &ptr);
	}

	/* Add number of items in special list */
	put_integer(o_ptr->num_special, &ptr);

	/* Add each item in special list */
	for (i = 0; i < o_ptr->num_special; i++)
	{
		/* Add item */
		put_integer(o_ptr->special[i], &ptr);
	}

	/* Add choice arguments */
	put_integer(o_ptr->arg1, &ptr);
	put_integer(o_ptr->arg2, &ptr);
	put_integer(o_ptr->arg3, &ptr);

	/* Finish message */
	finish_msg(msg, ptr);

	/* Send message to client */
	send_msg(c_ptr->fd, msg);
}

/*
 * Store the player's next choice.
 */
static void server_make_choice(game *g, int who, int type, int list[], int *nl,
                               int special[], int *ns, int arg1, int arg2,
                               int arg3)
{
	int sid = g->session_id;
	session *s_ptr = &s_list[sid];
	choice *o_ptr = &s_ptr->out[who];
	int i;

	/* Get wait mutex */
	pthread_mutex_lock(&s_ptr->wait_mutex);

	/* Mark player as being waited on */
	s_ptr->waiting[who] = WAIT_BLOCKED;

	/* Start counting ticks waiting */
	s_ptr->wait_ticks[who] = 0;

	/* Release wait mutex */
	pthread_mutex_unlock(&s_ptr->wait_mutex);

	/* Update waiting status */
	update_waiting(sid);

	/* Save type */
	o_ptr->type = type;

	/* Check for list */
	if (nl)
	{
		/* Copy length of list */
		o_ptr->num = *nl;

		/* Copy list items */
		for (i = 0; i < *nl; i++) o_ptr->list[i] = list[i];
	}
	else
	{
		/* No items in list */
		o_ptr->num = 0;
	}

	/* Check for special list */
	if (ns)
	{
		/* Copy length of special list */
		o_ptr->num_special = *ns;

		/* Copy special items */
		for (i = 0; i < *ns; i++) o_ptr->special[i] = special[i];
	}
	else
	{
		/* No special items */
		o_ptr->num_special = 0;
	}

	/* Copy choice arguments */
	o_ptr->arg1 = arg1;
	o_ptr->arg2 = arg2;
	o_ptr->arg3 = arg3;

	/* Ask client to respond */
	ask_client(sid, who);
}

/*
 * Handle a choice reply message from a client.
 */
static void handle_choice(int cid, char *ptr)
{
	session *s_ptr;
	player *p_ptr;
	int who, i, num, sid, pos;
	int *l_ptr;

	/* Get session ID from player */
	sid = c_list[cid].sid;

	/* Get session pointer */
	s_ptr = &s_list[sid];

	/* Loop over players in session */
	for (who = 0; who < s_ptr->num_users; who++)
	{
		/* Check for matching client ID */
		if (s_ptr->cids[who] == cid)
		{
			/* Get player pointer */
			p_ptr = &s_ptr->g.p[who];
			break;
		}
	}

	/* Get position in choice log that client is sending */
	pos = get_integer(&ptr);

	/* Get pointer to end of choice log */
	l_ptr = &p_ptr->choice_log[p_ptr->choice_size];

	/* Copy choice type to log */
	*l_ptr++ = get_integer(&ptr);

	/* printf("Received choice type %d position %d from %d, current size is %d.\n", *(l_ptr - 1), pos, cid, p_ptr->choice_size); */

	if (pos != p_ptr->choice_size) return;

	/* Copy return value */
	*l_ptr++ = get_integer(&ptr);

	/* Get number of items in list */
	num = get_integer(&ptr);

	/* Copy number of items to log */
	*l_ptr++ = num;

	/* Loop over list entries */
	for (i = 0; i < num; i++)
	{
		/* Copy item */
		*l_ptr++ = get_integer(&ptr);
	}

	/* Get number of special items in list */
	num = get_integer(&ptr);

	/* Copy number of special items to log */
	*l_ptr++ = num;

	/* Loop over special entries */
	for (i = 0; i < num; i++)
	{
		/* Copy item */
		*l_ptr++ = get_integer(&ptr);
	}

	/* Mark new size of choice log */
	p_ptr->choice_size = l_ptr - p_ptr->choice_log;

	/* Save choice log to database */
	db_save_choices(sid, who);

	/* Acquire wait mutex for session */
	pthread_mutex_lock(&s_ptr->wait_mutex);

	/* Check for blocked player */
	if (s_ptr->waiting[who] == WAIT_BLOCKED)
	{
		/* Mark player as ready */
		s_ptr->waiting[who] = WAIT_READY;
	}

	/* Mark time of activity */
	c_list[cid].last_active = time(NULL);

	/* Signal game thread to continue */
	pthread_cond_signal(&s_ptr->wait_cond);

	/* Unlock wait mutex */
	pthread_mutex_unlock(&s_ptr->wait_mutex);

	/* Update waiting status */
	update_waiting(sid);
}

/*
 * Handle preparation complete message.
 */
static void handle_prepare(int cid, char *ptr)
{
	session *s_ptr;
	int who, sid;

	/* Get session ID from player */
	sid = c_list[cid].sid;

	/* Get session pointer */
	s_ptr = &s_list[sid];

	/* Loop over players in session */
	for (who = 0; who < s_ptr->num_users; who++)
	{
		/* Check for matching client ID */
		if (s_ptr->cids[who] == cid) break;
	}

	/* Acquire wait mutex for session */
	pthread_mutex_lock(&s_ptr->wait_mutex);

	/* Check for preparing player */
	if (s_ptr->waiting[who] == WAIT_OPTION)
	{
		/* Mark player as ready */
		s_ptr->waiting[who] = WAIT_READY;
	}

	/* Signal game thread to continue */
	pthread_cond_signal(&s_ptr->wait_cond);

	/* Unlock wait mutex */
	pthread_mutex_unlock(&s_ptr->wait_mutex);

	/* printf("Received preparation complete from %d\n", who); */

	/* Update waiting status */
	update_waiting(sid);
}

/*
 * Set of functions called by game engine to notify/ask clients.
 */
decisions server_func =
{
	NULL,
	server_notify_rotation,
	server_make_choice,
	NULL,
	NULL,
	NULL
};

/*
 * Accept a new connection.
 */
static void accept_conn(int listen_fd)
{
	struct sockaddr_in peer_addr;
	socklen_t size = sizeof(struct sockaddr_in);
	int i;

	/* Loop through current list looking for an empty spot */
	for (i = 0; i < num_conn; i++)
	{
		/* Stop at empty connection */
		if (c_list[i].state == CS_EMPTY ||
		    c_list[i].state == CS_DISCONN) break;
	}

	/* Check for end of list reached */
	if (i == num_conn)
	{
		/* Increase count of active connections */
		num_conn++;
	}

	/* Accept connection */
	c_list[i].fd = accept(listen_fd, (struct sockaddr *)&peer_addr, &size);

	/* Check for failure */
	if (c_list[i].fd < 0)
	{
		/* Check for recoverable error */
		if (errno == EAGAIN) return;

		/* Print error and exit */
		perror("accept");
		exit(1);
	}

	/* Set state to initialized */
	c_list[i].state = CS_INIT;

	/* Save peer address for later */
	strcpy(c_list[i].addr, inet_ntoa(peer_addr.sin_addr));

	/* Reset timeout information */
	c_list[i].last_active = c_list[i].last_seen = time(NULL);

	/* Clear buffer length */
	c_list[i].buf_full = 0;

	/* Clear username */
	strcpy(c_list[i].user, "");

	/* Print message */
	printf("New connection %d from %s\n", i, c_list[i].addr);
}

/*
 * Kick a player from the server.
 */
static void kick_player(int cid, char *reason)
{
	int sid, i;

	/* Print message */
	printf("Kicking player %d for %s\n", cid, reason);

	/* Send goodbye message */
	send_msgf(c_list[cid].fd, MSG_GOODBYE, "s", reason);

	/* Set state to disconnected */
	c_list[cid].state = CS_DISCONN;

	/* Close connection */
	close(c_list[cid].fd);

	/* Clear file descriptor */
	c_list[cid].fd = -1;

	/* Send disconnect to everyone */
	send_player(cid);

	/* Check for no session joined */
	if (c_list[cid].sid < 0) return;

	/* Remember session player was in */
	sid = c_list[cid].sid;

	/* Remove from session */
	c_list[cid].sid = -1;

	/* Loop over connections in session */
	for (i = 0; i < s_list[sid].num_users; i++)
	{
		/* Check for match */
		if (s_list[sid].cids[i] == cid)
		{
			/* Clear connection ID */
			s_list[sid].cids[i] = -1;
		}
	}

	/* Send session details */
	send_session(sid);
}

/*
 * Add the given player to a session.
 */
static void join_game(int cid, int sid)
{
	session *s_ptr = &s_list[sid];
	int i;

	/* Mark active session in connection */
	c_list[cid].sid = sid;

	/* Check for user already in game */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Check for match */
		if (s_ptr->uids[i] == c_list[cid].uid)
		{
			/* Mark connection ID */
			s_ptr->cids[i] = cid;

			/* Resend session information */
			send_session(sid);

			/* Done */
			return;
		}
	}

	/* Add user ID to session */
	s_ptr->uids[s_ptr->num_users] = c_list[cid].uid;

	/* Add connection ID to session */
	s_ptr->cids[s_ptr->num_users] = cid;

	/* Count number of users */
	s_ptr->num_users++;

	/* Resend session details to everyone */
	send_session(sid);

	/* Remember join for later */
	db_join_game(c_list[cid].uid, s_ptr->gid);
}

/*
 * Remove a player from a game.
 */
static void leave_game(int sid, int who)
{
	session *s_ptr = &s_list[sid];
	int cid, uid;

	/* Get connection ID for player */
	cid = s_ptr->cids[who];

	/* Remember user ID of player */
	uid = s_ptr->uids[who];

	/* Connection is no longer in an active session */
	if (cid >= 0) c_list[cid].sid = -1;

	/* Keep count of number of users */
	s_ptr->num_users--;

	/* Move last player into empty spot */
	s_ptr->cids[who] = s_ptr->cids[s_ptr->num_users];
	s_ptr->uids[who] = s_ptr->uids[s_ptr->num_users];

	/* Resend session details to everyone */
	send_session(sid);

	/* Remember removal */
	db_leave_game(uid, s_ptr->gid);
}

/*
 * Switch a player to AI control.
 */
static void switch_ai(int sid, int who)
{
	session *s_ptr = &s_list[sid];
	char msg[1024];

	/* Mark player as AI */
	s_ptr->ai_control[who] = 1;

	/* Save AI control in database */
	db_save_seats(sid);

	/* Tell players about switch */
	sprintf(msg, "%s has been placed under AI control.\n",
	        s_ptr->g.p[who].name);

	/* Send message */
	message_add(&s_ptr->g, msg);

	/* Have AI answer most recent choice question */
	ask_client(sid, who);
}

/*
 * Run a started game.
 *
 * This function runs in a new thread.
 */
void *run_game(void *arg)
{
	session *s_ptr = (session *)arg;
	int i;

	/* Initialize wait mutex and condition variable */
	pthread_mutex_init(&s_ptr->wait_mutex, NULL);
	pthread_cond_init(&s_ptr->wait_cond, NULL);

	/* Initialize game */
	init_game(&s_ptr->g);

	/* Save session ID in game structure */
	s_ptr->g.session_id = s_ptr - s_list;

	/* Send meta status to clients */
	update_meta(s_ptr - s_list);

	/* Loop over players */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Skip players who are not connected */
		if (s_ptr->cids[i] < 0) continue;

		/* Give player a seat number */
		send_msgf(c_list[s_ptr->cids[i]].fd, MSG_SEAT, "d", i);
	}

	/* Begin game */
	begin_game(&s_ptr->g);

	/* Play game rounds until finished */
	while (game_round(&s_ptr->g));

	/* Score game */
	score_game(&s_ptr->g);

	/* Declare winner */
	declare_winner(&s_ptr->g);

	/* Send status to everyone */
	update_status(s_ptr - s_list);

	/* Loop over players */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Skip players who are not connected */
		if (s_ptr->cids[i] < 0) continue;

		/* Tell player that gawe is over */
		send_msgf(c_list[s_ptr->cids[i]].fd, MSG_GAMEOVER, "");

		/* Move player back to lobby state */
		c_list[s_ptr->cids[i]].state = CS_LOBBY;

		/* Remove player from session */
		c_list[s_ptr->cids[i]].sid = -1;
	}

	/* Mark session as finished */
	s_ptr->state = SS_DONE;

	/* Done */
	return NULL;
}

/*
 * Start a game session.
 */
static void start_session(int sid)
{
	session *s_ptr = &s_list[sid];
	char name[80];
	pthread_t t;
	int i;

	/* Copy paramaters to game structure */
	s_ptr->g.num_players = s_ptr->num_users;
	s_ptr->g.expanded = s_ptr->expanded;
	s_ptr->g.advanced = s_ptr->advanced;
	s_ptr->g.goal_disabled = s_ptr->disable_goal;
	s_ptr->g.takeover_disabled = s_ptr->disable_takeover;

	/* Save session ID in game structure */
	s_ptr->g.session_id = sid;

	/* Loop over players */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Set player interface function */
		s_ptr->g.p[i].control = &server_func;

		/* Create choice log */
		s_ptr->g.p[i].choice_log = (int *)malloc(sizeof(int) * 4096);

		/* Clear choice log size and position */
		s_ptr->g.p[i].choice_size = 0;
		s_ptr->g.p[i].choice_pos = 0;

		/* Get player's name */
		db_user_name(s_ptr->uids[i], name);

		/* Copy player's name */
		s_ptr->g.p[i].name = strdup(name);

		/* Clear waiting amount */
		s_ptr->wait_ticks[i] = 0;
	}

	/* Load game state from database, if able */
	if (!db_load_game_state(sid))
	{
		/* Initialize random byte pool */
		init_random_pool(sid);

		/* Save initial game state */
		db_save_game_state(sid);

		/* Save user positions */
		db_save_seats(sid);
	}

	/* Start a thread to run game */
	pthread_create(&t, NULL, run_game, (void *)s_ptr);
}

/*
 * Start any session that is in the "STARTED" state.
 *
 * This should only be called once during server setup.
 */
static void start_all_sessions(void)
{
	int i;

	/* Loop over sessions */
	for (i = 0; i < num_session; i++)
	{
		/* Skip sessions that aren't started */
		if (s_list[i].state != SS_STARTED) continue;

		/* Start session */
		start_session(i);
	}
}

/*
 * Handle a login message.
 */
static void handle_login(int cid, char *ptr)
{
	char user[1024], pass[1024], version[1024];
	int i, j;

	/* Ensure client is in INIT state */
	if (c_list[cid].state != CS_INIT)
	{
		/* Kick player to be safe */
		kick_player(cid, "Already logged in");
		return;
	}

	/* Pull strings from message */
	get_string(user, &ptr);
	get_string(pass, &ptr);
	get_string(version, &ptr);

	/* Check for too old version */
	if (strcmp(version, "0.6.1") < 0)
	{
		/* Send denied message */
		send_msgf(c_list[cid].fd, MSG_DENIED, "s",
			  "Client version too old");

		/* Done */
		return;
	}

	/* Check for too new version */
	if (strcmp(version, VERSION) > 0)
	{
		/* Send denied message */
		send_msgf(c_list[cid].fd, MSG_DENIED, "s",
			  "Client version too new");

		/* Done */
		return;
	}

	/* Check for weird length of username */
	if (strlen(user) < 2 || strlen(user) > 20)
	{
		/* Send denied message */
		send_msgf(c_list[cid].fd, MSG_DENIED, "s",
		          "Illegal username");

		/* Done */
		return;
	}

	/* Loop over username */
	for (ptr = user; *ptr; ptr++)
	{
		/* Check for illegal characters */
		if (!isalnum(*ptr) && *ptr != '_')
		{
			/* Send denied message */
			send_msgf(c_list[cid].fd, MSG_DENIED, "s",
			          "Illegal username");

			/* Done */
			return;
		}
	}

	/* Check for user already logged in */
	for (i = 0; i < num_conn; i++)
	{
		/* Skip empty connections */
		if (c_list[i].state == CS_EMPTY) continue;

		/* Skip disconnected entries */
		if (c_list[i].state == CS_DISCONN) continue;

		/* Check for matching username */
		if (!strcmp(c_list[i].user, user))
		{
			/* Send denied message */
			send_msgf(c_list[cid].fd, MSG_DENIED, "s",
			          "User already logged in");

			/* Done */
			return;
		}
	}

	/* Check for weird length of password */
	if (strlen(pass) < 1 || strlen(pass) > 20)
	{
		/* Send denied message */
		send_msgf(c_list[cid].fd, MSG_DENIED, "s",
		          "Illegal password length");

		/* Done */
		return;
	}

	/* Get user's ID and check password */
	c_list[cid].uid = db_user(user, pass);

	/* Check for bad password */
	if (c_list[cid].uid < 0)
	{
		/* Send denied message */
		send_msgf(c_list[cid].fd, MSG_DENIED, "s",
		          "Incorrect password");

		/* Done */
		return;
	}

	/* Print message */
	printf("Connection %d logged in with %s\n", cid, user);

	/* Set username */
	strcpy(c_list[cid].user, user);

	/* Set state to lobby */
	c_list[cid].state = CS_LOBBY;

	/* Tell client that login was successful */
	send_msgf(c_list[cid].fd, MSG_HELLO, "");

	/* Send welcome chat to client */
	send_msgf(c_list[cid].fd, MSG_CHAT, "ss", "",
	          "Welcome to the Race for the Galaxy server!");

	/* Clear session ID */
	c_list[cid].sid = -1;

	/* Loop over sessions */
	for (i = 0; i < num_session; i++)
	{
		/* Skip sessions that aren't in progress */
		if (s_list[i].state != SS_WAITING &&
		    s_list[i].state != SS_STARTED) continue;

		/* Loop over users in session */
		for (j = 0; j < s_list[i].num_users; j++)
		{
			/* Skip players that are controlled by the AI */
			if (s_list[i].ai_control[j]) continue;

			/* Check for user in active game */
			if (c_list[cid].uid == s_list[i].uids[j])
			{
				/* Rejoin game */
				join_game(cid, i);

				/* Tell client about joined session */
				send_msgf(c_list[cid].fd, MSG_JOINACK, "d", i);

				/* Check for started session */
				if (s_list[i].state == SS_STARTED)
				{
					/* Client is playing */
					c_list[cid].state = CS_PLAYING;

					/* Tell client that game has started */
					send_msgf(c_list[cid].fd, MSG_START,
					          "");

					/* Tell client about game state */
					update_meta(i);

					/* Give player a seat number */
					send_msgf(c_list[cid].fd, MSG_SEAT, "d",
					          j);

					/* Update waiting status */
					update_waiting(i);

					/* Ask player to answer last choice */
					ask_client(i, j);
				}
			}
		}
	}

	/* Send list of open sessions to client */
	send_open_sessions(cid);

	/* Tell everyone about new player */
	send_player(cid);

	/* Tell player about everyone else */
	send_all_players(cid);
}

/*
 * Handle a login message.
 */
static void handle_create(int cid, char *ptr)
{
	char pass[1024], desc[1024];
	int sid, i;

	/* Check for player already in game */
	if (c_list[cid].sid != -1) return;

	/* Loop through current list looking for an empty spot */
	for (sid = 0; sid < num_session; sid++)
	{
		/* Stop at empty session */
		if (s_list[sid].state == SS_EMPTY) break;
	}

	/* Check for end of list reached */
	if (sid == num_session)
	{
		/* Increase count of active sessions */
		num_session++;
	}

	/* Read game password and descripton */
	get_string(pass, &ptr);
	get_string(desc, &ptr);

	/* Check for no description */
	if (!strlen(desc))
	{
		/* Set description */
		strcpy(desc, "(none)");
	}

	/* Check for too-long password */
	if (strlen(pass) > 20)
	{
		/* Kick player */
		kick_player(cid, "Game password too long!");
		return;
	}

	/* Set session state */
	s_list[sid].state = SS_WAITING;

	/* Copy password and descripton to session */
	strcpy(s_list[sid].pass, pass);
	strcpy(s_list[sid].desc, desc);

	/* Copy creator's ID */
	s_list[sid].created = c_list[cid].uid;

	/* Clear user list */
	s_list[sid].num_users = 0;

	/* Loop over users */
	for (i = 0; i < MAX_PLAYER; i++)
	{
		/* Clear user and connection IDs */
		s_list[sid].cids[i] = s_list[sid].uids[i] = -1;

		/* Player is not under AI control */
		s_list[sid].ai_control[i] = 0;
	}

	/* Read game parameters */
	s_list[sid].min_player = get_integer(&ptr);
	s_list[sid].max_player = get_integer(&ptr);
	s_list[sid].expanded = get_integer(&ptr);
	s_list[sid].advanced = get_integer(&ptr);
	s_list[sid].disable_goal = get_integer(&ptr);
	s_list[sid].disable_takeover = get_integer(&ptr);

	/* Read preferred game speed */
	s_list[sid].speed = get_integer(&ptr);

	/* Insert game into database */
	s_list[sid].gid = db_new_game(sid);

	/* Have creating player join game */
	join_game(cid, sid);

	/* Send accepted message */
	send_msgf(c_list[cid].fd, MSG_JOINACK, "d", sid);
}

/*
 * Handle a join game message from a client.
 */
static void handle_join(int cid, char *ptr)
{
	char pass[1024];
	int sid;
	int i;

	/* Get session ID to join */
	sid = get_integer(&ptr);

	/* Check for game not in waiting state */
	if (s_list[sid].state != SS_WAITING)
	{
		/* Send denied message */
		send_msgf(c_list[cid].fd, MSG_JOINNAK, "s", "Game not open");
		return;
	}

	/* Loop over users */
	for (i = 0; i < s_list[sid].num_users; i++)
	{
		/* Check for player already joined */
		if (s_list[sid].uids[i] == c_list[cid].uid)
		{
			/* Send denied message */
			send_msgf(c_list[cid].fd, MSG_JOINNAK, "s",
			          "Already in this game");
			return;
		}
	}

	/* Check for player already in a game */
	if (c_list[cid].sid != -1)
	{
		/* Send denied message */
		send_msgf(c_list[cid].fd, MSG_JOINNAK, "s",
		          "Already joined a game");
		return;
	}

	/* Check for full game */
	if (s_list[sid].num_users >= s_list[sid].max_player)
	{
		/* Send denied message */
		send_msgf(c_list[cid].fd, MSG_JOINNAK, "s", "Game full");
		return;
	}

	/* Get client-supplied password */
	get_string(pass, &ptr);

	/* Check for password mismatch */
	if (strlen(s_list[sid].pass) > 0 && strcmp(pass, s_list[sid].pass))
	{
		/* Send denied message */
		send_msgf(c_list[cid].fd, MSG_JOINNAK, "s",
		          "Incorrect game password");
		return;
	}

	/* Join game */
	join_game(cid, sid);

	/* Send accepted message */
	send_msgf(c_list[cid].fd, MSG_JOINACK, "d", sid);
}

/*
 * Remove all players from a session and delete game.
 */
static void abandon_session(int sid)
{
	session *s_ptr = &s_list[sid];

	/* Loop until session is empty */
	while (s_ptr->num_users > 0)
	{
		/* Remove first user from session */
		leave_game(sid, 0);
	}

	/* Set game state to abandoned */
	s_ptr->state = SS_ABANDONED;

	/* Tell everyone about new state */
	send_session(sid);

	/* Save state to database */
	db_save_game_state(sid);

	/* Set game state to empty */
	s_ptr->state = SS_EMPTY;
}

/*
 * Handle a leave game message from a client.
 */
static void handle_leave(int cid, char *ptr)
{
	session *s_ptr;
	int i;

	/* Check for client not in session */
	if (c_list[cid].sid < 0) return;

	/* Get session pointer */
	s_ptr = &s_list[c_list[cid].sid];

	/* Loop over users in session */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Check for match */
		if (s_ptr->cids[i] == cid)
		{
			/* Check for session creator */
			if (s_ptr->created == c_list[cid].uid)
			{
				/* Abandon entire session */
				abandon_session(c_list[cid].sid);

				/* Done */
				return;
			}

			/* Leave session */
			leave_game(c_list[cid].sid, i);
			break;
		}
	}
}

/*
 * Handle a remove player message from client.
 */
static void handle_remove(int cid, char *ptr)
{
	session *s_ptr;
	int i, sid;
	char buf[1024], name[80];

	/* Read session ID from client */
	sid = get_integer(&ptr);

	/* Get session pointer */
	s_ptr = &s_list[sid];

	/* Ensure sender is owner of session */
	if (s_ptr->created != c_list[cid].uid)
	{
		/* Kick requester */
		kick_player(cid, "Tried to remove player when not game owner");
		return;
	}

	/* Get name of player to remove */
	get_string(buf, &ptr);

	/* Loop over players in session */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Get user's name */
		db_user_name(s_ptr->uids[i], name);

		/* Check for match */
		if (!strcmp(buf, name))
		{
			/* Check for connected player */
			if (s_ptr->cids[i] >= 0)
			{
				/* Tell player that they are out */
				send_msgf(c_list[s_ptr->cids[i]].fd, MSG_LEAVE,
				          "");
			}

			/* Remove player */
			leave_game(sid, i);

			/* Done */
			break;
		}
	}
}

/*
 * Handle a start game message from client.
 */
static void handle_start(int cid, char *ptr)
{
	session *s_ptr;
	int sid;
	int i;

	/* Read session ID from client */
	sid = get_integer(&ptr);

	/* Get session pointer */
	s_ptr = &s_list[sid];

	/* Ensure client has joined this session */
	if (c_list[cid].sid != sid)
	{
		/* Kick player */
		kick_player(cid, "Tried to start unjoined game");
		return;
	}

	/* Ensure client is game creator */
	if (s_ptr->created != c_list[cid].uid)
	{
		/* Kick player */
		kick_player(cid, "Tried to start unowned game");
		return;
	}

	/* Ensure game is waiting to start */
	if (s_ptr->state != SS_WAITING) return;

	/* Check for too few players */
	if (s_ptr->num_users < s_ptr->min_player) return;

	/* Mark session as started */
	s_ptr->state = SS_STARTED;

	/* Tell everyone that game is closed */
	send_session(sid);

	/* Loop over connected players */
	for (i = 0; i < s_ptr->num_users; i++)
	{
		/* Get connection ID */
		cid = s_ptr->cids[i];

		/* Skip offline players */
		if (cid < 0) continue;

		/* Set client status */
		c_list[cid].state = CS_PLAYING;

		/* Send game started message */
		send_msgf(c_list[cid].fd, MSG_START, "");
	}

	/* Message */
	printf("Starting game %s (session %d)\n", s_ptr->desc, sid);

	/* Initialize and run game */
	start_session(sid);
}

/*
 * Handle a chat message.
 */
static void handle_chat(int cid, char *ptr)
{
	char chat[1024], text[1024], msg[1024];
	int i;

	/* Read chat message */
	get_string(chat, &ptr);

	/* Check for sender in lobby */
	if (c_list[cid].state == CS_LOBBY)
	{
		/* Loop over all clients in lobby */
		for (i = 0; i < num_conn; i++)
		{
			/* Send chat to player */
			send_msgf(c_list[i].fd, MSG_CHAT, "ss",
			          c_list[cid].user, chat);
		}
	}
	else
	{
		/* Format chat with user's name */
		sprintf(text, "%s: %s\n", c_list[cid].user, chat);

		/* Start at beginning of message */
		ptr = msg;

		/* Create log message */
		start_msg(&ptr, MSG_LOG);

		/* Copy chat text to message */
		put_string(text, &ptr);

		/* Finish message */
		finish_msg(msg, ptr);

		/* Send to session */
		send_to_session(c_list[cid].sid, msg);
	}
}

/*
 * Handle a completed message from a client.
 */
static void handle_msg(int cid)
{
	char *ptr = c_list[cid].buf;
	int type, size;

	/* Read message type */
	type = get_integer(&ptr);

	/* Read size */
	size = get_integer(&ptr);

	/* Check for non-login message from client in INIT state */
	if (c_list[cid].state == CS_INIT && type != MSG_LOGIN)
	{
		/* Kick player */
		kick_player(cid, "Not logged in");
		return;
	}

	/* Switch on message type */
	switch (type)
	{
		/* Login */
		case MSG_LOGIN:

			/* Handle login message */
			handle_login(cid, ptr);
			break;

		/* Ping response */
		case MSG_PING:

			/* Do nothing */
			break;

		/* Create new session */
		case MSG_CREATE:

			/* Handle create message */
			handle_create(cid, ptr);
			break;

		/* Join session */
		case MSG_JOIN:

			/* Handle join message */
			handle_join(cid, ptr);
			break;

		/* Leave session */
		case MSG_LEAVE:

			/* Handle leave message */
			handle_leave(cid, ptr);
			break;

		/* Start game */
		case MSG_START:

			/* Handle start message */
			handle_start(cid, ptr);
			break;

		/* Remove player from game */
		case MSG_REMOVE:

			/* Handle remove message */
			handle_remove(cid, ptr);
			break;

		/* Choice made */
		case MSG_CHOOSE:

			/* Handle choice made */
			handle_choice(cid, ptr);
			break;

		/* Done preparing */
		case MSG_PREPARE:

			/* Handle preparation complete */
			handle_prepare(cid, ptr);
			break;

		/* Chat message */
		case MSG_CHAT:

			/* Handle chat message */
			handle_chat(cid, ptr);
			break;

		/* Unknown type */
		default:

			printf("Unknown message type %d\n", type);
			break;

			/* Kick player */
			kick_player(cid, "Unknown message type");
			break;
	}
}

/*
 * Handle incoming data from a client.
 */
static void handle_data(int cid)
{
	conn *c;
	char *ptr;
	int x;

	/* Get pointer to connection */
	c = &c_list[cid];

	/* Determine number of bytes to read */
	if (c->buf_full < 8)
	{
		/* Try to read 8 bytes for message header */
		x = 8;
	}
	else
	{
		/* Determine message size */
		ptr = c->buf + 4;
		x = get_integer(&ptr);
	}

	/* Check for too-long message */
	if (x > 1024)
	{
		/* Close connection */
		kick_player(cid, "Message too long");
		return;
	}

	/* Try to read as many bytes as needed */
	x = recv(c->fd, c->buf + c->buf_full, x - c->buf_full, 0);

	if (x < 0)
	{
		perror("recv");
		return;
	}

	/* Check for no bytes read */
	if (!x)
	{
		/* Client closed connection */
		kick_player(cid, "Client closed connection");
		return;
	}

	/* Add to amount read */
	c->buf_full += x;

	/* Check for complete message header */
	if (c->buf_full >= 8)
	{
		/* Determine length of incoming message */
		ptr = c->buf + 4;
		x = get_integer(&ptr);

		/* Check for illegally small message */
		if (x < 8)
		{
			/* Kick client */
			kick_player(cid, "Message too small");
			return;
		}

		/* Check for complete message */
		if (c->buf_full == x)
		{
			/* Handle message */
			handle_msg(cid);

			/* Clear buffer */
			c->buf_full = 0;
		}
	}

	/* Mark time of last data seen */
	c->last_seen = time(NULL);
}

/*
 * Perform simple housekeeping tasks every once in a while.
 */
static void do_housekeeping(void)
{
	session *s_ptr;
	time_t cur_time = time(NULL);
	int i, j, num;

	/* Loop over sessions */
	for (i = 0; i < num_session; i++)
	{
		/* Get session pointer */
		s_ptr = &s_list[i];

		/* Check for finished session */
		if (s_ptr->state == SS_DONE)
		{
			/* Save state */
			db_save_game_state(i);

			/* Mark session as empty once more */
			s_ptr->state = SS_EMPTY;
		}
	}

	/* Loop over clients */
	for (i = 0; i < num_conn; i++)
	{
		/* Skip empty/disconnected clients */
		if (c_list[i].state == CS_EMPTY ||
		    c_list[i].state == CS_DISCONN) continue;

		/* Check for no data from client in quite some time */
		if (cur_time - c_list[i].last_seen > 120)
		{
			/* Remove client */
			kick_player(i, "Timeout");
			continue;
		}

		/* Check for no recent data from client */
		if (cur_time - c_list[i].last_seen > 60)
		{
			/* Send client a ping */
			send_msgf(c_list[i].fd, MSG_PING, "");
		}
	}

	/* Loop over sessions */
	for (i = 0; i < num_session; i++)
	{
		/* Get session pointer */
		s_ptr = &s_list[i];

		/* Skip sessions that aren't in progress */
		if (s_ptr->state != SS_STARTED) continue;

		/* Assume nobody connected */
		num = 0;

		/* Loop over users in session */
		for (j = 0; j < s_ptr->num_users; j++)
		{
			/* Check for connected user */
			if (s_ptr->cids[j] >= 0) num++;
		}

		/* Don't set people to AI if no one connected */
		if (num == 0) continue;

		/* Grab mutex protecting wait array */
		pthread_mutex_lock(&s_ptr->wait_mutex);

		/* Loop over users in session */
		for (j = 0; j < s_ptr->num_users; j++)
		{
			/* Skip users who we are not waiting on */
			if (s_ptr->waiting[j] != WAIT_BLOCKED) continue;

			/* Don't count ticks of player if only one connected */
			if (num == 1 && s_ptr->cids[j] >= 0) continue;

			/* Add to wait count */
			s_ptr->wait_ticks[j]++;

			/* Check for too much time elasped */
			if (s_ptr->wait_ticks[j] > 5)
			{
				/* Check for player connected */
				if (s_ptr->cids[j] >= 0)
				{
					/* Kick player */
					kick_player(s_ptr->cids[j],
					            "Set to AI due to delay");
				}

				/* Release wait mutex */
				pthread_mutex_unlock(&s_ptr->wait_mutex);

				/* Set player to AI */
				switch_ai(i, j);

				/* Reacquire wait mutex */
				pthread_mutex_lock(&s_ptr->wait_mutex);
			}
		}

		/* Release wait mutex */
		pthread_mutex_unlock(&s_ptr->wait_mutex);
	}
}

/*
 * Initialize connection to database, open main listening socket, then loop
 * forever waiting for incoming data on connections.
 */
int main(void)
{
	struct sockaddr_in listen_addr;
	struct timeval sel_timeout;
	fd_set readfds;
	int listen_fd;
	int i, n;
	my_bool reconnect = 1;
	time_t last_housekeep = 0;

	/* Read card library */
	read_cards();

	/* Initialize database library */
	mysql = mysql_init(NULL);

	/* Check for error */
	if (!mysql)
	{
		/* Print error and exit */
		printf("Couldn't initialize database library!\n");
		exit(1);
	}

	/* Attempt to connect to database server */
	if (!mysql_real_connect(mysql, NULL, "rftg", NULL, "rftg", 0, NULL, 0))
	{
		/* Print error and exit */
		printf("Database connection: %s\n", mysql_error(mysql));
		exit(1);
	}

	/* Reconnect automatically when connection to database is lost */
	mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect);
 
	/* Read game states from database */
	db_load_sessions();
	db_load_attendance();

	/* Start sessions that were running previously */
	start_all_sessions();

	/* Ignore SIGPIPE when writing to a closed socket */
	signal(SIGPIPE, SIG_IGN);

	/* Create main socket for new connections */
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);

	/* Check for error */
	if (listen_fd < 0)
	{
		/* Message and exit */
		perror("socket");
		exit(1);
	}

	/* Create address of listening socket */
	listen_addr.sin_family = AF_INET;
	listen_addr.sin_port = htons(16309);
	listen_addr.sin_addr.s_addr = 0;

	/* Bind socket to local port */
	if (bind(listen_fd, (struct sockaddr *)&listen_addr,
	          sizeof(struct sockaddr_in)) < 0)
	{
		/* Message and exit */
		perror("bind");
		exit(1);
	}

	/* Establish listening queue */
	if (listen(listen_fd, 10) < 0)
	{
		/* Message and exit */
		perror("listen");
		exit(1);
	}

	/* Loop forever */
	while (1)
	{
		/* Clear set of connections to listen on */
		FD_ZERO(&readfds);

		/* Add main listening socket to list */
		FD_SET(listen_fd, &readfds);

		/* Track biggest file descriptor */
		n = listen_fd;

		/* Loop over active connections */
		for (i = 0; i < num_conn; i++)
		{
			/* Check for still active */
			if (c_list[i].fd > 0)
			{
				/* Add descriptor to set */
				FD_SET(c_list[i].fd, &readfds);

				/* Track biggest file descriptor */
				if (c_list[i].fd > n) n = c_list[i].fd;
			}
		}

		/* Wait no more than 10 seconds */
		sel_timeout.tv_sec = 10;
		sel_timeout.tv_usec = 0;

		/* Wait for activity on any connection */
		select(n + 1, &readfds, NULL, NULL, &sel_timeout);

		/* Check for new incoming connection */
		if (FD_ISSET(listen_fd, &readfds))
		{
			/* Accept new connection */
			accept_conn(listen_fd);
		}

		/* Loop over active connections */
		for (i = 0; i < num_conn; i++)
		{
			/* Check for still active */
			if (c_list[i].fd > 0)
			{
				/* Check for activity on this connection */
				if (FD_ISSET(c_list[i].fd, &readfds))
				{
					/* Handle incoming data */
					handle_data(i);
				}
			}
		}

		/* Check time since last housekeeping */
		if (time(NULL) - last_housekeep > 10)
		{
			/* Perform housekeeping */
			do_housekeeping();

			/* Remember time */
			last_housekeep = time(NULL);
		}
	}
}
