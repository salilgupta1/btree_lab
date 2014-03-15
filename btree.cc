#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "btree.h"

KeyValuePair::KeyValuePair()
{}


KeyValuePair::KeyValuePair(const KEY_T &k, const VALUE_T &v) : 
  key(k), value(v)
{}


KeyValuePair::KeyValuePair(const KeyValuePair &rhs) :
  key(rhs.key), value(rhs.value)
{}


KeyValuePair::~KeyValuePair()
{}


KeyValuePair & KeyValuePair::operator=(const KeyValuePair &rhs)
{
  return *( new (this) KeyValuePair(rhs));
}

BTreeIndex::BTreeIndex(SIZE_T keysize, 
		       SIZE_T valuesize,
		       BufferCache *cache,
		       bool unique) 
{
  superblock.info.keysize=keysize;
  superblock.info.valuesize=valuesize;
  buffercache=cache;
  // note: ignoring unique now
}

BTreeIndex::BTreeIndex()
{
  // shouldn't have to do anything
}


//
// Note, will not attach!
//
BTreeIndex::BTreeIndex(const BTreeIndex &rhs)
{
  buffercache=rhs.buffercache;
  superblock_index=rhs.superblock_index;
  superblock=rhs.superblock;
}

BTreeIndex::~BTreeIndex()
{
}


BTreeIndex & BTreeIndex::operator=(const BTreeIndex &rhs)
{
  return *(new(this)BTreeIndex(rhs));
}


ERROR_T BTreeIndex::AllocateNode(SIZE_T &n)
{
  n=superblock.info.freelist;

  if (n==0) { 
    return ERROR_NOSPACE;
  }

  BTreeNode node;

  node.Unserialize(buffercache,n);

  assert(node.info.nodetype==BTREE_UNALLOCATED_BLOCK);

  superblock.info.freelist=node.info.freelist;

  superblock.Serialize(buffercache,superblock_index);

  buffercache->NotifyAllocateBlock(n);

  return ERROR_NOERROR;
}


ERROR_T BTreeIndex::DeallocateNode(const SIZE_T &n)
{
  BTreeNode node;

  node.Unserialize(buffercache,n);

  assert(node.info.nodetype!=BTREE_UNALLOCATED_BLOCK);

  node.info.nodetype=BTREE_UNALLOCATED_BLOCK;

  node.info.freelist=superblock.info.freelist;

  node.Serialize(buffercache,n);

  superblock.info.freelist=n;

  superblock.Serialize(buffercache,superblock_index);

  buffercache->NotifyDeallocateBlock(n);

  return ERROR_NOERROR;

}

ERROR_T BTreeIndex::Attach(const SIZE_T initblock, const bool create)
{
  ERROR_T rc;

  superblock_index=initblock;
  assert(superblock_index==0);

  if (create) {
    // build a super block, root node, and a free space list
    //
    // Superblock at superblock_index
    // root node at superblock_index+1
    // free space list for rest
    BTreeNode newsuperblock(BTREE_SUPERBLOCK,
			    superblock.info.keysize,
			    superblock.info.valuesize,
			    buffercache->GetBlockSize());
    newsuperblock.info.rootnode=superblock_index+1;
    newsuperblock.info.freelist=superblock_index+2;
    newsuperblock.info.numkeys=0;

    buffercache->NotifyAllocateBlock(superblock_index);

    rc=newsuperblock.Serialize(buffercache,superblock_index);

    if (rc) { 
      return rc;
    }
    
    BTreeNode newrootnode(BTREE_ROOT_NODE,
			  superblock.info.keysize,
			  superblock.info.valuesize,
			  buffercache->GetBlockSize());
    newrootnode.info.rootnode=superblock_index+1;
    newrootnode.info.freelist=superblock_index+2;
    newrootnode.info.numkeys=0;

    buffercache->NotifyAllocateBlock(superblock_index+1);

    rc=newrootnode.Serialize(buffercache,superblock_index+1);

    if (rc) { 
      return rc;
    }

    for (SIZE_T i=superblock_index+2; i<buffercache->GetNumBlocks();i++) { 
      BTreeNode newfreenode(BTREE_UNALLOCATED_BLOCK,
			    superblock.info.keysize,
			    superblock.info.valuesize,
			    buffercache->GetBlockSize());
      newfreenode.info.rootnode=superblock_index+1;
      newfreenode.info.freelist= ((i+1)==buffercache->GetNumBlocks()) ? 0: i+1;
      
      rc = newfreenode.Serialize(buffercache,i);

      if (rc) {
	return rc;
      }

    }
  }

  // OK, now, mounting the btree is simply a matter of reading the superblock 

  return superblock.Unserialize(buffercache,initblock);
}
    

ERROR_T BTreeIndex::Detach(SIZE_T &initblock)
{
  return superblock.Serialize(buffercache,superblock_index);
}
 

ERROR_T BTreeIndex::LookupOrUpdateInternal(const SIZE_T &node,
					   const BTreeOp op,
					   const KEY_T &key,
					   VALUE_T &value)
{
  BTreeNode b;
  ERROR_T rc;
  SIZE_T offset;
  KEY_T testkey;
  SIZE_T ptr;

  rc= b.Unserialize(buffercache,node);

  if (rc!=ERROR_NOERROR) { 
    return rc;
  }

  switch (b.info.nodetype) { 
  case BTREE_ROOT_NODE:
  case BTREE_INTERIOR_NODE:
    // Scan through key/ptr pairs
    //and recurse if possible
    for (offset=0;offset<b.info.numkeys;offset++) { 
      rc=b.GetKey(offset,testkey);
      if (rc) {  return rc; }
      if (key<testkey) {
      	// OK, so we now have the first key that's larger
      	// so we ned to recurse on the ptr immediately previous to 
      	// this one, if it exists
      	rc=b.GetPtr(offset,ptr);
      	if (rc) { return rc; }
      	return LookupOrUpdateInternal(ptr,op,key,value);
      }
    }
    // if we got here, we need to go to the next pointer, if it exists
    if (b.info.numkeys>0) { 
      rc=b.GetPtr(b.info.numkeys,ptr);
      if (rc) { return rc; }
      return LookupOrUpdateInternal(ptr,op,key,value);
    } else {
      // There are no keys at all on this node, so nowhere to go
      return ERROR_NONEXISTENT;
    }
    break;
  case BTREE_LEAF_NODE:
    // Scan through keys looking for matching value
    for (offset=0;offset<b.info.numkeys;offset++) { 
      rc=b.GetKey(offset,testkey);
      if (rc) {  return rc; }
      if (testkey==key) { 
	if (op==BTREE_OP_LOOKUP) { 
	  return b.GetVal(offset,value);
	} else { 
	  // BTREE_OP_UPDATE
	  // WRITE ME
        rc = b.SetVal(offset, value);
        if(rc) {return rc;}
        rc = b.Serialize(buffercache, node);
        return rc;
	}
      }
    }
    return ERROR_NONEXISTENT;
    break;
  default:
    // We can't be looking at anything other than a root, internal, or leaf
    return ERROR_INSANE;
    break;
  }  

  return ERROR_INSANE;
}


static ERROR_T PrintNode(ostream &os, SIZE_T nodenum, BTreeNode &b, BTreeDisplayType dt)
{
  KEY_T key;
  VALUE_T value;
  SIZE_T ptr;
  SIZE_T offset;
  ERROR_T rc;
  unsigned i;

  if (dt==BTREE_DEPTH_DOT) { 
    os << nodenum << " [ label=\""<<nodenum<<": ";
  } else if (dt==BTREE_DEPTH) {
    os << nodenum << ": ";
  } else {
  }

  switch (b.info.nodetype) { 
  case BTREE_ROOT_NODE:
  case BTREE_INTERIOR_NODE:
    if (dt==BTREE_SORTED_KEYVAL) {
    } else {
      if (dt==BTREE_DEPTH_DOT) { 
      } else { 
	os << "Interior: ";
      }
      for (offset=0;offset<=b.info.numkeys;offset++) { 
	rc=b.GetPtr(offset,ptr);
	if (rc) { return rc; }
	os << "*" << ptr << " ";
	// Last pointer
	if (offset==b.info.numkeys) break;
	rc=b.GetKey(offset,key);
	if (rc) {  return rc; }
	for (i=0;i<b.info.keysize;i++) { 
	  os << key.data[i];
	}
	os << " ";
      }
    }
    break;
  case BTREE_LEAF_NODE:
    if (dt==BTREE_DEPTH_DOT || dt==BTREE_SORTED_KEYVAL) { 
    } else {
      os << "Leaf: ";
    }
    for (offset=0;offset<b.info.numkeys;offset++) { 
      if (offset==0) { 
	// special case for first pointer
	rc=b.GetPtr(offset,ptr);
	if (rc) { return rc; }
	if (dt!=BTREE_SORTED_KEYVAL) { 
	  os << "*" << ptr << " ";
	}
      }
      if (dt==BTREE_SORTED_KEYVAL) { 
	os << "(";
      }
      rc=b.GetKey(offset,key);
      if (rc) {  return rc; }
      for (i=0;i<b.info.keysize;i++) { 
	os << key.data[i];
      }
      if (dt==BTREE_SORTED_KEYVAL) { 
	os << ",";
      } else {
	os << " ";
      }
      rc=b.GetVal(offset,value);
      if (rc) {  return rc; }
      for (i=0;i<b.info.valuesize;i++) { 
	os << value.data[i];
      }
      if (dt==BTREE_SORTED_KEYVAL) { 
	os << ")\n";
      } else {
	os << " ";
      }
    }
    break;
  default:
    if (dt==BTREE_DEPTH_DOT) { 
      os << "Unknown("<<b.info.nodetype<<")";
    } else {
      os << "Unsupported Node Type " << b.info.nodetype ;
    }
  }
  if (dt==BTREE_DEPTH_DOT) { 
    os << "\" ]";
  }
  return ERROR_NOERROR;
}
  
ERROR_T BTreeIndex::Lookup(const KEY_T &key, VALUE_T &value)
{
  return LookupOrUpdateInternal(superblock.info.rootnode, BTREE_OP_LOOKUP, key, value);
}



bool BTreeIndex::IsNodeFull(const SIZE_T node)
{
    BTreeNode b;
    b.Unserialize(buffercache, node);

    switch (b.info.nodetype) {
        case BTREE_ROOT_NODE:
        case BTREE_INTERIOR_NODE:
            return (b.info.GetNumSlotsAsInterior() == b.info.numkeys);
        case BTREE_LEAF_NODE:
            return (b.info.GetNumSlotsAsLeaf() == b.info.numkeys);
    }
    cout << "No such node type in btree!" << endl;
    return false;
}


/// Mechanism to Split a full btree Node into two
/// Places the correct set of keys to the correct left and right nodes
ERROR_T BTreeIndex::SplitNode(const SIZE_T node, SIZE_T &newNode, KEY_T &splitKey)
{
    BTreeNode left;
    SIZE_T numLeftKeys, numRightKeys;
    ERROR_T rc;
    left.Unserialize(buffercache, node);
    BTreeNode right = left;

    if ((rc = AllocateNode(newNode)))
        return rc;
    if ((rc = right.Serialize(buffercache, newNode)))
        return rc;
    
    if (left.info.nodetype == BTREE_LEAF_NODE) {
        numLeftKeys = (left.info.numkeys + 2) / 2;
        numRightKeys = left.info.numkeys - numLeftKeys;

        left.GetKey(numLeftKeys - 1, splitKey);

        char *src = left.ResolveKeyVal(numLeftKeys); 
        char *dest = right.ResolveKeyVal(0);

        memcpy(dest, src, numRightKeys * (left.info.keysize + left.info.valuesize));
    } else {
        numLeftKeys = left.info.numkeys / 2;
        numRightKeys = left.info.numkeys - numLeftKeys - 1;
        
        left.GetKey(numLeftKeys, splitKey);

        char *src = left.ResolvePtr(numLeftKeys + 1);
        char *dest = right.ResolvePtr(0);
        
        memcpy(dest, src, numRightKeys * (left.info.keysize + sizeof(SIZE_T)) + sizeof(SIZE_T));
    }
    left.info.numkeys = numLeftKeys;
    right.info.numkeys = numRightKeys;

    if ((rc = left.Serialize(buffercache, node)))
        return rc;
    return right.Serialize(buffercache, newNode);
}

/// PLaces the new key valaue pair at a new node
ERROR_T BTreeIndex::AddKeyValuePair(const SIZE_T node, const KEY_T &key, const VALUE_T &value, SIZE_T newNode)
{
    BTreeNode b;
    b.Unserialize(buffercache, node);
    KEY_T testkey;
    SIZE_T entriesToCopy;
    SIZE_T numkeys = b.info.numkeys;
    SIZE_T i;
    ERROR_T rc;
    SIZE_T entrySize;

    switch (b.info.nodetype) {
        case BTREE_INTERIOR_NODE:
            entrySize = b.info.keysize + sizeof(SIZE_T);
            break;
        case BTREE_LEAF_NODE:
            entrySize = b.info.keysize + b.info.valuesize;
            break;
        case BTREE_ROOT_NODE:
        default:
            return ERROR_INSANE;
    }

    b.info.numkeys++;
    if (numkeys > 0) {
        for (i=0, entriesToCopy = numkeys;i < numkeys; i++, entriesToCopy--) {
            if ((rc = b.GetKey(i, testkey)))
                return rc;
            if (key < testkey) {
                void *src = b.ResolveKey(i);
                void *dest = b.ResolveKey(i + 1);
                memmove(dest, src, entriesToCopy * entrySize);
                if (b.info.nodetype == BTREE_LEAF_NODE) {
                    if ((rc = b.SetKey(i, key)) || (rc = b.SetVal(i, value)))
                        return rc;
                } else {
                    if ((rc = b.SetKey(i, key)) || (rc = b.SetPtr(i + 1, newNode)))
                        return rc;
                }
                break;
            }
            if (i == numkeys - 1) {
                if (b.info.nodetype == BTREE_LEAF_NODE) {
                    if ((rc = b.SetKey(numkeys, key)) || (rc = b.SetVal(numkeys, value)))
                        return rc;
                } else {
                    if ((rc = b.SetKey(numkeys, key)) || (rc = b.SetPtr(numkeys+1, newNode)))
                        return rc;
                }
                break;
            }
        }
    } else if ((rc = b.SetKey(0, key)) || (rc = b.SetVal(0, value))) return rc;
    return b.Serialize(buffercache, node);
}

/// Recursively adds the moved nodes to a new block
ERROR_T BTreeIndex::RecursivePlacement(SIZE_T node, SIZE_T parent, const KEY_T &key, const VALUE_T &value)
{
    BTreeNode b;
    ERROR_T rc;
    SIZE_T i;
    KEY_T testkey;
    SIZE_T ptr;
    
    SIZE_T newNode;
    KEY_T splitKey;

    b.Unserialize(buffercache, node); 
    // Store block data
    switch (b.info.nodetype) {
        case BTREE_ROOT_NODE:
        case BTREE_LEAF_NODE:
            return AddKeyValuePair(node, key, value, 0);
            break;
        case BTREE_INTERIOR_NODE:
            for (i=0;i<b.info.numkeys;i++)
            {
                rc=b.GetKey(i,testkey);
                if (rc) {  return rc; }
                if (key<testkey) {
                    rc=b.GetPtr(i,ptr);
                    if (rc) { return rc; }
                    rc=RecursivePlacement(ptr, node, key, value);
                    if (rc) { return rc; }
                    if (IsNodeFull(ptr)) {
                        rc = SplitNode(ptr, newNode, splitKey);
                        if (rc) { return rc; }
                        return AddKeyValuePair(node, splitKey, VALUE_T(), newNode);
                    } else {
                        return rc;
                    }
                }
            }
            if (b.info.numkeys>0) {
                rc=b.GetPtr(b.info.numkeys,ptr);
                if (rc) { return rc; }
                rc=RecursivePlacement(ptr, node, key, value);
                if (rc) { return rc; }
                if (IsNodeFull(ptr)) {
                    rc = SplitNode(ptr, newNode, splitKey);
                    if (rc) { return rc; }
                    return AddKeyValuePair(node, splitKey, VALUE_T(), newNode);
                } else {
                    return rc;
                }
            } else {
                return ERROR_NONEXISTENT;
            }
            break;
        default:
            return ERROR_INSANE;
            break;
    }  
    return ERROR_INSANE;
}

/// Inserting a key value pair in the btree
ERROR_T BTreeIndex::Insert(const KEY_T &key, const VALUE_T &value)
{
    ERROR_T error;
    BTreeNode root;
    root.Unserialize(buffercache,superblock.info.rootnode);

    if (root.info.numkeys == 0) { 
        BTreeNode leaf(BTREE_LEAF_NODE, 
            superblock.info.keysize,
            superblock.info.valuesize,
            buffercache->GetBlockSize());
        
        SIZE_T leftNode;
        SIZE_T rightNode;
        if ((error = AllocateNode(leftNode)) != ERROR_NOERROR) return error;
        if ((error = AllocateNode(rightNode)) != ERROR_NOERROR) return error;
        leaf.Serialize(buffercache, leftNode); 
        leaf.Serialize(buffercache, rightNode);
        root.info.numkeys += 1;
        root.SetKey(0, key);
        root.SetPtr(0, leftNode);
        root.SetPtr(1, rightNode);
        root.Serialize(buffercache, superblock.info.rootnode);
    } 
    VALUE_T temp;
    SIZE_T oldRoot=superblock.info.rootnode, newNode;
    KEY_T splitKey;

    BTreeNode interior(BTREE_INTERIOR_NODE,superblock.info.keysize,  superblock.info.valuesize,
        buffercache->GetBlockSize());

    if (ERROR_NONEXISTENT == Lookup(key, temp)) {
        error = RecursivePlacement(superblock.info.rootnode, superblock.info.rootnode, key, value);
        if (IsNodeFull(superblock.info.rootnode)) {
            SplitNode(oldRoot, newNode, splitKey);
            interior.Unserialize(buffercache, oldRoot);
            interior.Serialize(buffercache, oldRoot);
            interior.Unserialize(buffercache, newNode);
            interior.Serialize(buffercache, newNode);

            if ((error = AllocateNode(superblock.info.rootnode)) != ERROR_NOERROR)
                return error;
            root.info.numkeys = 1;
            root.SetKey(0, splitKey);
            root.SetPtr(0, oldRoot);
            root.SetPtr(1, newNode);
            root.Serialize(buffercache, superblock.info.rootnode);
        }
        return error;
    }
    else
        return ERROR_CONFLICT;

  return ERROR_UNIMPL;
}
  
ERROR_T BTreeIndex::Update(const KEY_T &key, const VALUE_T &value)
{
    VALUE_T val = value;
    return LookupOrUpdateInternal(superblock.info.rootnode, BTREE_OP_UPDATE, key, val);
    return ERROR_NOERROR;
}

  
ERROR_T BTreeIndex::Delete(const KEY_T &key)
{
  // Optional - Extra Credit
  return ERROR_UNIMPL;
}

  
//
//
// DEPTH first traversal
// DOT is Depth + DOT format
//
ERROR_T BTreeIndex::DisplayInternal(const SIZE_T &node,
				    ostream &o,
				    BTreeDisplayType display_type) const
{
  KEY_T testkey;
  SIZE_T ptr;
  BTreeNode b;
  ERROR_T rc;
  SIZE_T offset;

  rc= b.Unserialize(buffercache,node);

  if (rc!=ERROR_NOERROR) { 
    return rc;
  }

  rc = PrintNode(o,node,b,display_type);
  
  if (rc) { return rc; }

  if (display_type==BTREE_DEPTH_DOT) { 
    o << ";";
  }

  if (display_type!=BTREE_SORTED_KEYVAL) {
    o << endl;
  }

  switch (b.info.nodetype) { 
  case BTREE_ROOT_NODE:
  case BTREE_INTERIOR_NODE:
    if (b.info.numkeys>0) { 
      for (offset=0;offset<=b.info.numkeys;offset++) { 
	rc=b.GetPtr(offset,ptr);
	if (rc) { return rc; }
	if (display_type==BTREE_DEPTH_DOT) { 
	  o << node << " -> "<<ptr<<";\n";
	}
	rc=DisplayInternal(ptr,o,display_type);
	if (rc) { return rc; }
      }
    }
    return ERROR_NOERROR;
    break;
  case BTREE_LEAF_NODE:
    return ERROR_NOERROR;
    break;
  default:
    if (display_type==BTREE_DEPTH_DOT) { 
    } else {
      o << "Unsupported Node Type " << b.info.nodetype ;
    }
    return ERROR_INSANE;
  }

  return ERROR_NOERROR;
}


ERROR_T BTreeIndex::Display(ostream &o, BTreeDisplayType display_type) const
{
  ERROR_T rc;
  if (display_type==BTREE_DEPTH_DOT) { 
    o << "digraph tree { \n";
  }
  rc=DisplayInternal(superblock.info.rootnode,o,display_type);
  if (display_type==BTREE_DEPTH_DOT) { 
    o << "}\n";
  }
  return ERROR_NOERROR;
}


ERROR_T BTreeIndex::SanityCheck() const
{
      ERROR_T rc;
    SIZE_T totalKeys;
    // Check if keys in order within node
    // Also, count up keys in leaf nodes
    rc=NodeCheck(superblock.info.rootnode, totalKeys);
    if(rc){
        return rc;
    }
    // return insane tree when totalkeys in leaf nodes isn't the same as the numkeys of superblock
    if (totalKeys != superblock.info.numkeys) {
        return ERROR_INSANE;
    }
    return ERROR_NOERROR;
}

ERROR_T BTreeIndex::NodeCheck(const SIZE_T &node, SIZE_T &totalKeys) const
{
    return ERROR_NOERROR;
    KEY_T key;
    SIZE_T ptr;
    KEY_T prev;
    SIZE_T offset;
    int first=1;
    ERROR_T rc;
    BTreeNode b;
    totalKeys = 0;

    rc= b.Unserialize(buffercache,node);
    if(rc) {
        return rc;
    }
    switch(b.info.nodetype){
        case BTREE_ROOT_NODE:
        case BTREE_INTERIOR_NODE:
            if ((int)(b.info.GetNumSlotsAsInterior()*(2./3.)) <= b.info.numkeys) { 
                return ERROR_INSANE;
            }
            if (b.info.numkeys>0) {
                for (offset=0;offset<=b.info.numkeys;offset++) {
                    // Recurse down
                    rc=b.GetPtr(offset,ptr);
                    if (rc) { return rc; }
                    rc=NodeCheck(ptr,totalKeys);
                    if (rc) { return rc; }
                  }
            }
            return ERROR_NOERROR;
            break;
        case BTREE_LEAF_NODE:
            if ((int)(b.info.GetNumSlotsAsLeaf()*(2./3.)) <= b.info.numkeys){
                return ERROR_INSANE;
            }
            if (b.info.numkeys>0) {
                // keep track of number of keys
                totalKeys += b.info.numkeys;
                for (offset=0;offset<=b.info.numkeys;offset++) {
                    rc=b.GetKey(offset,key);
                    if ( rc ) { return rc; }
                    if(first==1)
                    {
                        prev = key; first = 0;
                    } 
                    else 
                    {if( prev==key|| prev<key){
                            prev=key;
                        }
                        else
                        {return ERROR_INSANE;}
                    }
                }
            }
            return ERROR_NOERROR;
            break;
        default:
      return ERROR_NOERROR;
    }
}


ostream & BTreeIndex::Print(ostream &os) const
{
  // WRITE ME
  Display(os, BTREE_SORTED_KEYVAL);
  return os;
}