﻿#pragma once
#include "SingleAgentSolver.h"


class AStarNode: public LLNode
{
public:
	// define a typedefs for handles to the heaps (allow up to quickly update a node in the heap)
	typedef pairing_heap< AStarNode*, compare<LLNode::compare_node> >::handle_type open_handle_t;
	open_handle_t open_handle;

	AStarNode() : LLNode() {}

	AStarNode(int loc, int g_val, int h_val, LLNode* parent, int timestep, bool in_openlist = false) :
		LLNode(loc, g_val, h_val, parent, timestep, in_openlist) {}


	~AStarNode() {}

	// The following is used by for generating the hash value of a nodes
	struct NodeHasher
	{
		size_t operator()(const AStarNode* n) const
		{
			size_t loc_hash = std::hash<int>()(n->location);
			size_t timestep_hash = std::hash<int>()(n->timestep);
			return (loc_hash ^ (timestep_hash << 1));
		}
	};

	// The following is used for checking whether two nodes are equal
	// we say that two nodes, s1 and s2, are equal if
	// both are non-NULL and agree on the id and timestep
	struct eqnode
	{
		bool operator()(const AStarNode* s1, const AStarNode* s2) const
		{
			return (s1 == s2) || (s1 && s2 &&
                        s1->location == s2->location &&
                        s1->timestep == s2->timestep );
		}
	};
};


class SpaceTimeAStar: public SingleAgentSolver
{
public:
	// find path by time-space A* search
	// Returns a shortest path that satisfies the constraints of the give node  while
	// minimizing the number of internal conflicts (that is conflicts with known_paths for other agents found so far).
	// lowerbound is an underestimation of the length of the path in order to speed up the search.
	Path findOptimalPath();
	Path findSuboptimalPath();  // return the path and the lowerbound

	string getName() const { return "AStar"; }

	SpaceTimeAStar(const Instance& instance, int agent):
		SingleAgentSolver(instance, agent) {}

private:
	// define typedefs and handles for heap
	typedef pairing_heap< AStarNode*, compare<AStarNode::compare_node> > heap_open_t;
	heap_open_t open_list;

	// define typedef for hash_map
	typedef unordered_set<AStarNode*, AStarNode::NodeHasher, AStarNode::eqnode> hashtable_t;
	hashtable_t allNodes_table;

	// Updates the path datamember
	void updatePath(const LLNode* goal, vector<PathEntry> &path);
	inline AStarNode* popNode();
	inline void pushNode(AStarNode* node);
	void releaseNodes();

};
