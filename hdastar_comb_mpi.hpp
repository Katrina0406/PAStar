/*
 * hdastar.hpp
 *
 *  Created on: Jul 17, 2014
 *      Author: yuu
 */

#ifndef HDASTAR_COMB_MPI_HPP_
#define HDASTAR_COMB_MPI_HPP_

#include <vector>
//#include <pthread.h>
//#include <atomic>
#include <unistd.h>
#include <math.h>
#include <algorithm>
#include <array>

#include "../search.hpp"
#include "../utils.hpp"
#include "hashtbl_mpi.hpp"
//#include "hashtblbig.hpp"
#include "../heap.hpp"
//#include "pool.hpp"
//#include "buffer.hpp"

//#include "naive_heap.hpp"

#include "../zobrist.hpp"
//#include "trivial_hash.hpp"
//#include "random_hash.hpp"

#include <mpi.h>
// DELAY 10,000,000 -> 3000 nodes per second
//#define DELAY 0

#define MPI_MSG_NODE 0 // node
#define MPI_MSG_INCM 1 // for updating incumbent. message is unsigned int.
#define MPI_MSG_TERM 2 // for termination.message is empty.
#define MPI_MSG_FTERM 3 // broadcast this message when process terminated.
template<class D> class HDAstarCombMPI: public SearchAlg<D> {

	struct Node {
		unsigned int f, g;
		unsigned int zbr; // zobrist value. stored here for now. Also the size is char for now.
		int openind;
//		Node *parent; // TODO: somehow need to hold this info
		HashEntry<Node> hentry;
		char pop;
		typename D::PackedState packed;

		const bool pred(Node *o) {
			if (f == o->f)
				return g > o->g;
			return f < o->f;
		}
		const unsigned int size() {
			return 25 + packed.byteSize();
		}
		HashEntry<Node> &hashentry() {
			return hentry;
		}
		typename D::PackedState &key() {
			return packed;
		}
	};

	/**
	 * Char representation of the Node:
	 *
	 * 0-3:   f
	 * 4-7:   g
	 * 8-11:  zbr
	 * 12-15: openind
	 * 16-19: hash (hentry.hash)
	 * 20:    pop
	 * 21-24: packed size
	 * 25-:   packed state representation
	 *
	 */

	void uintToChars(unsigned int p, unsigned char* d) {
		int n = sizeof p;
		for (int y = 0; n-- > 0; y++)
			d[y] = (p >> (n * 8)) & 0xff;
	}
	template<typename T>
	void charsToData(T& p, unsigned char* d) {
		int n = sizeof p;
		p = 0;
		for (int i = 0; i < n; ++i) {
			p += (T) d[i] << (8 * (n - i - 1));
		}
	}

	void nodeToChars(Node* n, unsigned char* d) {
		uintToChars(n->f, &(d[0]));
		uintToChars(n->g, &(d[4]));
		uintToChars(n->zbr, &(d[8]));
		uintToChars(n->openind, &(d[12]));
		uintToChars(n->hentry.hash, &(d[16]));
		d[20] = n->pop;
		unsigned int state_size = n->packed.byteSize();
		uintToChars(state_size, &(d[21]));

		n->packed.stateToChars(&(d[25]));
	}

	void charsToNode(unsigned char* d, Node& n) {
		charsToData(n.f, &(d[0]));
		charsToData(n.g, &(d[4]));
		charsToData(n.zbr, &(d[8]));
		charsToData(n.openind, &(d[12]));
		charsToData(n.hentry.hash, &(d[16]));
		n.pop = d[20];
		n.packed.charsToState(&(d[25]));
	}

	void charsToNodes(unsigned char* d, const unsigned int dsize,
			std::vector<Node*>& nodes) {
		unsigned int dp = 0;
		unsigned int nsize = 0;
		while (dp < dsize) {
			Node* p = new Node();
			charsToNode(&(d[dp]), *p);
			nodes.push_back(p);
			dp += p->size();
		}
	}

	void print(Node* n) {
		printf("state = %ud\n", n->packed.word);
		printf("f,g = %ud, %ud\n", n->f, n->g);
		printf("hash = %ud\n", n->hentry.hash);
		printf("openind = %ud\n", n->openind);
		printf("zbr = %ud\n", n->zbr);

	}

	std::vector<typename D::State> path;
	typename D::State init;
	int tnum;

	int incumbent;
	bool tot_terminate;

	int id; // MPI_RANK

	int outgo_threshold;

	int force_income = 0;
	int force_outgo = 0;

	std::vector<unsigned int> expd_distribution;
	std::vector<unsigned int> gend_distribution;
	std::vector<unsigned int> duplicates;

	std::vector<unsigned int> self_pushes;

	double wall0 = 0; // ANALYZE_FTRACE

	std::vector<int> fvalues; // OUTSOURCING
	std::vector<int> open_sizes;

	////////////////////////
	/// MPI
	////////////////////////
	unsigned char* mpi_buffer;

	int overrun;
	unsigned int closedlistsize;
	unsigned int openlistsize;
	unsigned int initmaxcost;

//	bool isTimed = false;
//	double timer = 0.0;

public:

	HDAstarCombMPI(D &d, int tnum_, int outgo_threshold_ = 0, int overrun_ = 0,
			unsigned int closedlistsize = 1105036, unsigned int openlistsize =
					100, unsigned int maxcost = 1000000) :
			SearchAlg<D>(d), tnum(tnum_), incumbent(maxcost), outgo_threshold(
					outgo_threshold_), overrun(overrun_), closedlistsize(
					closedlistsize), openlistsize(openlistsize), initmaxcost(
					maxcost) {

		expd_distribution.resize(tnum);
		gend_distribution.resize(tnum);
		duplicates.resize(tnum);
		self_pushes.resize(tnum);


		fvalues.resize(tnum);
		open_sizes.resize(tnum);
	}

	void set_closedlistsize(unsigned int closedlistsize) {
		this->closedlistsize = closedlistsize;
	}

	void thread_search() {
		MPI_Status status;
		MPI_Request request;
		HashTableMPI<typename D::PackedState, Node> closed(closedlistsize);

		Heap<Node> open(openlistsize, overrun);

		// If the buffer is locked when the thread pushes a node,
		// stores it locally and pushes it afterward.
		// This array would be allocated in heap rather than stack.
		// Therefore, not the best optimized way to do.
		// Also we need to fix it to compile in clang++.
		std::vector<std::vector<Node>> outgo_buffer;
		outgo_buffer.resize(tnum);

		std::vector<Node> tmp;
		uint expd_here = 0;
		uint gend_here = 0;
		int max_outgo_buffer_size = 0;
		int max_income_buffer_size = 0;
		int duplicate_here = 0;
		int current_f = 0;
		double lapse;
		int useless = 0;
		int fval = -1;
		// How many of the nodes sent to itself.
		// If this high, then lower communication overhead.
		unsigned int self_push = 0;
		unsigned int over_incumbent_count = 0;
		unsigned int no_work_iteration = 0;
		double init_time = walltime();
		printf("id = %d\n", id);
		int loop_count = 0;
		bool has_sent_first_term = false; // this field is only for id == 0.

		if (id == 0) {
			// wrap a new node.
			Node* n = new Node;
			{
				n->g = 0;
				n->f = this->dom.h(init);
				n->pop = -1;
				//			n->parent = 0;
				this->dom.pack(n->packed, init);
			}
			open.push(n);
		}

		while (true) {
			++loop_count;
			if (loop_count % 10000000 == 0) {
				printf("loop: %d, expd: %d, gend: %d\n", loop_count, expd_here,
						gend_here);
			}

			int has_received = 0;

			Node *n;

			if (this->isTimed) {
				double t = walltime() - init_time;
				if (t > this->timer) {
					printf("Terminated due to timer\n");
					break;
				}
			}


			//////////////////////////////////////////////////
			// INCOME QUEUE OPERATION
			//////////////////////////////////////////////////
			receiveNodes(open);
			has_received = 0;
			MPI_Iprobe(MPI_ANY_SOURCE, MPI_MSG_INCM, MPI_COMM_WORLD,
					&has_received, &status);
			if (has_received) {
				int newincm;
				MPI_Recv(&newincm, 1, MPI_INT, MPI_ANY_SOURCE, MPI_MSG_INCM,
						MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				if (incumbent > newincm) {
					incumbent = newincm;
				}
				printf("incumbent = %d\n", incumbent);
			}

			if (open.isemptyunder(incumbent)) {
				flushOutgoBuffers(outgo_buffer, 0);
				if (terminationDetection(has_sent_first_term)) {
					printf("%d terminated\n", id);
					break;
				}
				continue;
			}

			// Reset as we got a new work node.
			has_sent_first_term = false;

			n = static_cast<Node*>(open.pop());

#ifdef ANALYZE_LAPSE
			endlapse(lapse, "openlist");
#endif
#ifdef ANALYZE_FTRACE
			int newf = open.minf();
			Logfvalue* lg = new Logfvalue(walltime() - wall0, n->f, n->f - n->g);
			logfvalue[id].push_back(*lg);
			if (fvalues[id] != newf) {
//				printf("ftrace %d %d %f\n", id, fvalues[id],
//						walltime() - wall0);
				fvalues[id] = newf;
			}
#endif // ANALYZE_FTRACE
#ifdef ANALYZE_GLOBALF
			if (n->f != fvalues[id]) {
				fvalues[id] = n->f;
				int min = *std::min_element(fvalues, fvalues+tnum);

				if (min != globalf) {
					globalf = min;
					printf("globalf %d %d %f\n", id, min, walltime() - wall0);
				}
			}
#endif
#ifdef ANALYZE_LAPSE
			startlapse(&lapse); // closed list
#endif

			Node *duplicate = closed.find(n->key());
			if (duplicate) {
				if (duplicate->f <= n->f) {
					dbgprintf ("Discarded\n");
					delete n;
					continue;
#ifdef ANALYZE_DUPLICATE
				} else {
					duplicate_here++;
#endif // ANALYZE_DUPLICATE
				}
				// Node access here is unnecessary duplicates.
				++duplicate_here;
			}
			//	}
#ifdef ANALYZE_LAPSE
			endlapse(lapse, "closedlist");
#endif
#ifdef OUTSOURCING
			if ((n->thrown < 5) && (expd_here > 600) && outsourcing(n, id)) {
				if (n->thrown == 0) {
					closed.add(n);
				}
#ifdef ANALYZE_OUTSOURCING
				outsource_pushed++;
#endif
				dbgprintf("Out sourced a node\n");
				continue;
			}
#endif // OUTSOURCING
			typename D::State state;
			this->dom.unpack(state, n->packed);


#ifdef ANALYZE_ORDER
			if (fval != n->f) {
				fval = n->f;
				LogNodeOrder* ln = new LogNodeOrder(globalOrder.fetch_add(1),
						state.tiles, fval, open.getsize());
				lognodeorder[id].push_back(*ln);
			} else {
				LogNodeOrder* ln = new LogNodeOrder(globalOrder.fetch_add(1),
						state.tiles, -1, open.getsize());
				lognodeorder[id].push_back(*ln);
			}
#endif // ANALYZE_ORDER
			if (this->dom.isgoal(state)) {
				std::vector<typename D::State> newpath;
				int length = n->g;
				printf("cost = %u\n", n->g);

				if (incumbent > n->g) {
					incumbent = n->g;
					path = newpath;
				}

				for (int i = 0; i < tnum; ++i) {
					if (i != id) {
						MPI_Isend(&incumbent, 1, MPI_INT, i, MPI_MSG_INCM,
								MPI_COMM_WORLD, &request);
					}
				}
				continue;
			}
#ifdef OUTSOURCING
			if (n->thrown == 0) {
				closed.add(n);
			}
#else
			closed.add(n);
#endif
			expd_here++;
			//		printf("expd: %d\n", id);

#ifdef ANALYZE_LAPSE
			startlapse(&lapse);
#endif
			unsigned int nops = this->dom.nops(state);
			for (int i = 0; i < nops; i++) {
				int op = this->dom.nthop(state, i);
				if (op == n->pop) {
					continue;
				}
				useless += uselessCalc(useless);
				Edge<D> e = this->dom.apply(state, op);
				Node* next = wrap(state, n, e.cost, e.pop);
				gend_here++;
				next->zbr = this->dom.dist_hash(state);

				unsigned int zbr = next->zbr % tnum;

				// If the node belongs to itself, just push to this open list.
				if (zbr == id) {

					++self_push;
					open.push(next);
#ifdef SEMISYNC
					// Synchronous communication to avoid search overhead
					else if (outgo_buffer[zbr].size() > outgo_threshold) {
						income_buffer[zbr].lock();
						income_buffer[zbr].push_with_lock(next);
						income_buffer[zbr].push_all_with_lock(outgo_buffer[zbr]);
//					printf("%d", __LINE__);
						income_buffer[zbr].release_lock();
						outgo_buffer[zbr].clear();
#ifdef ANALYZE_SEMISYNC
						++force_outgo;
//					printf("semisync = %d to %d\n", id, zbr);
#endif // ANALYZE_SEMISYNC
					}
#endif // SEMISYNC
				} else {
					outgo_buffer[zbr].push_back(*next);
#ifdef ANALYZE_OUTGO
					int size = outgo_buffer[zbr].size();
					if (size > max_outgo_buffer_size) {
						max_outgo_buffer_size = size;
					}
#endif // ANALYZE_OUTGO
				}

				this->dom.undo(state, e);
			}

			flushOutgoBuffers(outgo_buffer, outgo_threshold);

#ifdef ANALYZE_LAPSE
			endlapse(lapse, "expand");
#endif
		}

		printf("%d terminating...\n", id);
		printf("expd: %d\n", expd_here);
		printf("gend: %d\n", gend_here);
		printf("sent: %d\n", gend_here - self_push);

		if (gend_here > 0) {
			printf("CO: %.3f\n", (gend_here - self_push) / gend_here);
		}

		// Solved (maybe)

		this->wtime = walltime();
		this->ctime = cputime();

		this->open_sizes[id] = open.getsize();

		// From here, you can dump every comments as it would not be included in walltime & cputime.

		dbgprintf ("pathsize = %lu\n", path.size());
		this->expd_distribution[id] = expd_here;
		this->gend_distribution[id] = gend_here;

#ifdef ANALYZE_OUTGO
		this->max_outgo += max_outgo_buffer_size;
#endif
#ifdef ANALYZE_INCOME
		this->max_income += max_income_buffer_size;
#endif
		this->duplicates[id] = duplicate_here;

		this->self_pushes[id] = self_push;

		dbgprintf ("END\n");
		return;
	}

	void receiveNodes(Heap<Node>& open) {
		MPI_Status status;
		int has_received = 0;
		MPI_Iprobe(MPI_ANY_SOURCE, MPI_MSG_NODE, MPI_COMM_WORLD, &has_received,
				&status);
		while (has_received != 0) {
			int income_size = 0;
			MPI_Get_count(&status, MPI_BYTE, &income_size);

			unsigned char *d = new unsigned char[income_size];
			MPI_Recv(d, income_size, MPI_BYTE, MPI_ANY_SOURCE, MPI_MSG_NODE,
					MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			std::vector<Node*> nodes;
			charsToNodes(d, income_size, nodes);

			for (int i = 0; i < nodes.size(); ++i) {
				open.push(nodes[i]);
			}

			delete[] d;
			nodes.clear();

			has_received = 0;
			MPI_Iprobe(MPI_ANY_SOURCE, MPI_MSG_NODE, MPI_COMM_WORLD,
					&has_received, &status);
		}
	}

	// check if its optimal.
	bool terminationDetection(bool& has_sent_first_term) {
		MPI_Status status;
		int has_received = 0;

		MPI_Iprobe(MPI_ANY_SOURCE, MPI_MSG_FTERM, MPI_COMM_WORLD, &has_received,
				MPI_STATUS_IGNORE);
		if (has_received) {
			printf("received fterm\n");
			return true;
		}

		if (id == 0 && !has_sent_first_term) {
			// TODO: how can we maintain the memory while MPI_Isend is executing?
			unsigned char term = 0;
			MPI_Bsend(&term, 1, MPI_BYTE, (id + 1) % tnum, MPI_MSG_TERM,
					MPI_COMM_WORLD);
			printf("sent first term\n");
			has_sent_first_term = true;
			sleep(1);
		}

		has_received = 0;
		MPI_Iprobe((id - 1) % tnum, MPI_MSG_TERM, MPI_COMM_WORLD, &has_received,
				&status);
		if (has_received) {
			int term = 0;
			MPI_Recv(&term, 1, MPI_BYTE, (id - 1) % tnum, MPI_MSG_TERM,
					MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			if (id == 0) {
				++term;
			}

			printf("%d received term %d\n", id, term);
			MPI_Bsend(&term, 1, MPI_BYTE, (id + 1) % tnum, MPI_MSG_TERM,
					MPI_COMM_WORLD);
			if (term > 1) {
				return true;
			}

			MPI_Iprobe((id - 1) % tnum, MPI_MSG_TERM, MPI_COMM_WORLD,
					&has_received, &status);

			// This while loop is here to flush all messages
			while (has_received) {
				MPI_Recv(&term, 1, MPI_BYTE, (id - 1) % tnum, MPI_MSG_TERM,
						MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				if (term == 1) {
					printf("%d received term %d\n", id, term);
					MPI_Bsend(&term, 1, MPI_BYTE, (id + 1) % tnum, MPI_MSG_TERM,
							MPI_COMM_WORLD);
					return true;
				}
				has_received = 0;
				MPI_Iprobe((id - 1) % tnum, MPI_MSG_TERM, MPI_COMM_WORLD,
						&has_received, &status);
			}

		}
		return false;
	}

	void flushOutgoBuffers(std::vector<std::vector<Node>>& outgo_buffer,
			int thresould) {
		for (int i = 0; i < tnum; ++i) {
			if (i != id && outgo_buffer[i].size() > thresould) {
				unsigned int totsize = 0;
				for (int j = 0; j < outgo_buffer[i].size(); ++j) {
					totsize += outgo_buffer[i][j].size();
				}
				unsigned char* d = new unsigned char[totsize];
				unsigned char* dp = d;
				for (int j = 0; j < outgo_buffer[i].size(); ++j) {
					Node send = outgo_buffer[i][j];
					nodeToChars(&send, dp);
//						MPI_Isend(d, send.size(), MPI_BYTE, i, MPI_MSG_NODE,
//								MPI_COMM_WORLD, &request);
					// TODO: asynchronzie it like try_lock.
					MPI_Bsend(dp, send.size(), MPI_BYTE, i, MPI_MSG_NODE,
							MPI_COMM_WORLD);
//						printf("sent f,g = %d %d\n", send.f, send.g);
					dp += send.size();
				}
				delete[] d;
				outgo_buffer[i].clear();
			}
		}
	}

	std::vector<typename D::State> search(typename D::State &init) {

		printf("search\n");
		int world_size;
		MPI_Comm_size(MPI_COMM_WORLD, &world_size);
		MPI_Comm_rank(MPI_COMM_WORLD, &id);
		printf("id=%d\n", id);

		unsigned int node_size = 32;
		unsigned int threshold = 100;
		// Buffer
		unsigned int buffer_size = (node_size + MPI_BSEND_OVERHEAD) * world_size
				* 100
				+ (node_size * threshold + MPI_BSEND_OVERHEAD) * world_size
						* 10000;
		unsigned int buffer_max = 400000000; // 400 MB TODO: not sure this is enough or too much.
		if (buffer_size > buffer_max) {
			buffer_size = 400000000;
		}

		printf("buffersize=%u\n", buffer_size);
		mpi_buffer = new unsigned char[buffer_size];
		std::fill(mpi_buffer, mpi_buffer + buffer_size, 0);
		MPI_Buffer_attach((void *) mpi_buffer, buffer_size);

		this->init = init;

		wall0 = walltime();
#ifdef OUTSOURCING
		for (int i = 0; i < tnum; ++i) {
			fvalues[i] = n->f;
		}
#endif

		MPI_Barrier(MPI_COMM_WORLD);
		printf("start\n");

		thread_search();

		for (int i = 0; i < tnum; ++i) {
			this->expd += expd_distribution[i];
			this->gend += gend_distribution[i];
			this->lpush += self_pushes[i];
			this->dup += duplicates[i];
		}

		if (this->isTimed) {
			return path;
		}

#ifdef ANALYZE_INCOME
		printf("average of max_income_buffer_size = %d\n", max_income / tnum);
#endif
#ifdef ANALYZE_OUTGO
		printf("average of max_outgo_buffer_size = %d\n", max_outgo / tnum);
#endif
#ifdef ANALYZE_DUPLICATE
		printf("duplicate_distribution =");
		for(int i=0; i<tnum;++i) {
			printf(" %d", duplicates[i]);
		}
		printf("\n");
#endif
#ifdef ANALYZE_DISTRIBUTION
		printf("expansion balance = %f\n", load_balance(expd_distribution));
		printf("expansion stddev = %f\n",
				analyze_distribution(expd_distribution));
		printf("expansion_distribution =");
		for(int i=0; i<tnum;++i) {
			printf(" %d", expd_distribution[i]);
		}
		printf("\n");

		printf("generation balance = %f\n", load_balance(gend_distribution));
		printf("generation stddev = %f\n",
				analyze_distribution(gend_distribution));
		printf("generation =");
		for(int i=0; i<tnum;++i) {
			printf(" %d", gend_distribution[i]);
		}
		printf("\n");
#endif
		printf("forcepush incomebuffer = %d\n", force_income);
		printf("forcepush outgobuffer = %d\n", force_outgo);

#ifdef ANALYZE_FTRACE
		for (int i = 0; i < tnum; ++i) {
			for (int j = 0; j < this->logfvalue[i].size(); ++j) {
				printf("ftrace %f %d %d %d\n", logfvalue[i][j].walltime,
						i, logfvalue[i][j].fvalue, logfvalue[i][j].hvalue);
			}
		}

#endif

#ifdef ANALYZE_ORDER
		for (int id = 0; id < tnum; ++id) {
			for (int i = 0; i < lognodeorder[id].size(); ++i) {
				printf("%d %d %016lx %d %d\n", id, lognodeorder[id][i].globalOrder, lognodeorder[id][i].packedState,
						lognodeorder[id][i].fvalue, lognodeorder[id][i].openlistSize);
			}
		}
#endif // ANALYZE_ORDER
#ifdef ANALYZE_OUTSOURCING
		printf("outsource node pushed = %d\n", outsource_pushed);
#endif
		termination();
		return path;
	}


	inline Node *wrap(typename D::State &s, Node *p, int c, int pop) {
		Node *n = new Node();
		n->g = c;
		if (p)
			n->g += p->g;
		n->f = n->g + this->dom.h(s);
		n->pop = pop;
		this->dom.pack(n->packed, s);
		return n;
	}


	int termination() {

		printf("barrier %d\n", id);
		MPI_Barrier(MPI_COMM_WORLD);


		MPI_Status status;
		printf("Flushing all incoming messages... ");

		unsigned char* dummy = new unsigned char[10 * 10000];

		int has_received = 1;
		while (has_received) {
			has_received = 0;
			MPI_Iprobe(MPI_ANY_SOURCE, MPI_MSG_FTERM, MPI_COMM_WORLD, &has_received,
					MPI_STATUS_IGNORE);
			if (has_received) {
				MPI_Recv(dummy, 0, MPI_BYTE, MPI_ANY_SOURCE, MPI_MSG_FTERM,
						MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			}
		}

		printf("FTERM... ");

		has_received = 1;
		while (has_received) {
			has_received = 0;
			MPI_Iprobe(MPI_ANY_SOURCE, MPI_MSG_INCM, MPI_COMM_WORLD, &has_received,
					MPI_STATUS_IGNORE);
			if (has_received) {
				MPI_Recv(dummy, 1, MPI_INT, MPI_ANY_SOURCE, MPI_MSG_INCM,
						MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			}
		}
		printf("INCM... ");

		has_received = 1;
		while (has_received) {
			has_received = 0;
			MPI_Iprobe(MPI_ANY_SOURCE, MPI_MSG_NODE, MPI_COMM_WORLD, &has_received,
					&status);
			if (has_received) {
				int source = status.MPI_SOURCE;
				int d_size;
				MPI_Get_count(&status, MPI_BYTE, &d_size); // TODO: = node_size?
				MPI_Recv(dummy, d_size, MPI_BYTE, source, MPI_MSG_NODE,
						MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			}
		}
		printf("NODE... ");

		has_received = 1;
		while (has_received) {
			has_received = 0;
			MPI_Iprobe(MPI_ANY_SOURCE, MPI_MSG_TERM, MPI_COMM_WORLD, &has_received,
					MPI_STATUS_IGNORE);
			if (has_received) {
				MPI_Recv(dummy, 1, MPI_INT, MPI_ANY_SOURCE, MPI_MSG_TERM,
						MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			}
		}
		printf("TERM... ");



		printf("done!\n");
		printf("Buffer_detach %d\n", id);

		int buffer_size;
		MPI_Buffer_detach(&mpi_buffer, &buffer_size);
		delete[] mpi_buffer;

		printf("finalize %d\n", id);

		MPI_Finalize();
		printf("finalize done%d\n", id);

		return 0;
	}



	void print_state(typename D::State state) {
		for (int i = 0; i < D::Ntiles; ++i) {
			printf("%d ", state.tiles[i]);
		}
		printf("\n");
	}

// MAX / AVERAGE
	double load_balance(std::vector<unsigned int> distribution) {
		double avrg = 0, max = 0;
		for (int i = 0; i < tnum; ++i) {
			avrg += distribution[i];
			if (max < distribution[i]) {
				max = distribution[i];
			}
		}
		avrg /= tnum;
		return max / avrg;
	}

	double analyze_distribution(std::vector<unsigned int> distribution) {
		double avrg = 0;
		for (int i = 0; i < tnum; ++i) {
			avrg += distribution[i];
		}
		avrg /= tnum;
		double var = 0;
		for (int i = 0; i < tnum; ++i) {
			var += (avrg - distribution[i]) * (avrg - distribution[i]);
		}
		var /= tnum;
		double stddev = sqrt(var);
//		for (int i = 0; i < tnum; ++i) {
//			printf("%d ", distribution[i]);
//		}
		return stddev;
	}

//	void setTimer(double timer) {
//		isTimed = true;
//		this->timer = timer;
//	}

	void unsetTimer() {
		this->isTimed = false;

	}

// CAUTIONS!
// These lapsing methods uses cputime methods.
// It won't take into account for locks or any other synchronous methods.
	void startlapse(double* laptime) {
		*laptime = clock();
//		printf("st %f\n", laptime);
	}

	void endlapse(double previous, const char* comment) {
		double wt = clock() - previous;
//		printf("end %f\n",wt);
		printf("lapse %s %.0f\n", comment, wt);
	}

	int uselessCalc(int useless) {
//		static const int zero = 0;
//		int test = 0;
		int uselessLocal = 0;
		for (int i = 0; i < 0; ++i) {
//			++test;
			int l = 0;
			l += useless;
			l = 2 * l - uselessLocal;
			if (l != 0) {
				uselessLocal = l % useless;
			} else {
				uselessLocal = useless * 2;
			}
		}
//		printf("DELAY = %d\n", test);
		return uselessLocal;
	}

public:

	std::vector<unsigned int> getExpansions() {
		return expd_distribution;
	}
	std::vector<unsigned int> getGenerations() {
		return gend_distribution;
	}
	std::vector<unsigned int> getSelfPushes() {
		return self_pushes;
	}
	std::vector<unsigned int> getDuplicates() {
		return duplicates;
	}

}
;

#endif /* HDASTAR_HPP_ */
