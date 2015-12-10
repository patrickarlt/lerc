/*
Copyright 2015 Esri

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

A local copy of the license and additional notices are located with the
source distribution at:

  http://www.github.com/esri/lerc/LICENSE
  http://www.github.com/esri/lerc/NOTICE

Contributors:  Thomas Maurer
*/

#include "Huffman.h"
#include "BitStuffer2.h"
#include <queue>

using namespace std;
using namespace LercNS;

// -------------------------------------------------------------------------- ;

bool Huffman::ComputeCodes(const vector<int>& histo)
{
  if (histo.empty() || histo.size() >= m_maxHistoSize)
    return false;

  priority_queue<Node, vector<Node>, less<Node> > pq;

  int numNodes = 0;

  int size = (int)histo.size();
  for (int i = 0; i < size; i++)    // add all leaf nodes
    if (histo[i] > 0)
      pq.push(Node((short)i, histo[i]));

  if (pq.size() < 2)    // histo has only 0 or 1 bin that is not empty; quit Huffman and give it to Lerc
    return false;

  while (pq.size() > 1)    // build the Huffman tree
  {
    Node* child0 = new Node(pq.top());
    numNodes++;
    pq.pop();
    Node* child1 = new Node(pq.top());
    numNodes++;
    pq.pop();
    pq.push(Node(child0, child1));
  }

  m_codeTable.resize(size);
  memset(&m_codeTable[0], 0, size * sizeof(m_codeTable[0]));
  if (!pq.top().TreeToLUT(0, 0, m_codeTable))    // fill the LUT
    return false;

  //pq.top().child0->FreeTree(numNodes);    // free all the nodes
  //pq.top().child1->FreeTree(numNodes);
  pq.top().FreeTree(numNodes);

  if (numNodes != 0)    // check the ref count
    return false;

  return true;
}

// -------------------------------------------------------------------------- ;

bool Huffman::ComputeCompressedSize(const std::vector<int>& histo, int& numBytes, double& avgBpp) const
{
  if (histo.empty() || histo.size() >= m_maxHistoSize)
    return false;

  numBytes = 0;
  if (!ComputeNumBytesCodeTable(numBytes))    // header and code table
    return false;

  int numBits = 0, numElem = 0;
  int size = (int)histo.size();
  for (int i = 0; i < size; i++)
    if (histo[i] > 0)
    {
      numBits += histo[i] * m_codeTable[i].first;
      numElem += histo[i];
    }

  int numUInts = ((((numBits + 7) >> 3) + 3) >> 2) + 1;    // add one more as the decode LUT can read ahead
  numBytes += 4 * numUInts;    // data huffman coded
  avgBpp = 8 * numBytes / (double)numElem;

  return true;
}

// -------------------------------------------------------------------------- ;

bool Huffman::SetCodes(const vector<pair<short, unsigned int> >& codeTable)
{
  if (codeTable.empty() || codeTable.size() >= m_maxHistoSize)
    return false;

  m_codeTable = codeTable;
  return true;
}

// -------------------------------------------------------------------------- ;

bool Huffman::WriteCodeTable(Byte** ppByte) const
{
  if (!ppByte)
    return false;

  int i0, i1, maxLen;
  if (!GetRange(i0, i1, maxLen))
    return false;

  int size = (int)m_codeTable.size();
  vector<unsigned int> dataVec(i1 - i0, 0);

  for (int i = i0; i < i1; i++)
  {
    int k = GetIndexWrapAround(i, size);
    dataVec[i - i0] = (unsigned int)m_codeTable[k].first;
  }

  // header
  vector<int> intVec;
  intVec.push_back(2);    // version
  intVec.push_back(size);
  intVec.push_back(i0);   // code range
  intVec.push_back(i1);

  Byte* ptr = *ppByte;

  for (size_t i = 0; i < intVec.size(); i++)
  {
    *((int*)ptr) = intVec[i];
    ptr += sizeof(int);
  }

  BitStuffer2 bitStuffer2;
  if (!bitStuffer2.EncodeSimple(&ptr, dataVec))    // code lengths, bit stuffed
    return false;

  if (!BitStuffCodes(&ptr, i0, i1))    // variable length codes, bit stuffed
    return false;
  
  *ppByte = ptr;
  return true;
}

// -------------------------------------------------------------------------- ;

bool Huffman::ReadCodeTable(const Byte** ppByte)
{
  if (!ppByte || !(*ppByte))
    return false;

  const Byte* ptr = *ppByte;

  int version = *((int*)ptr);    // version
  ptr += sizeof(int);

  if (version != 2)
    return false;

  vector<int> intVec(4, 0);
  for (size_t i = 1; i < intVec.size(); i++)
  {
    intVec[i] = *((int*)ptr);
    ptr += sizeof(int);
  }

  int size = intVec[1];
  int i0 = intVec[2];
  int i1 = intVec[3];

  if (i0 >= i1 || size > (int)m_maxHistoSize)
    return false;

  vector<unsigned int> dataVec(i1 - i0, 0);
  BitStuffer2 bitStuffer2;
  if (!bitStuffer2.Decode(&ptr, dataVec))    // unstuff the code lengths
    return false;

  m_codeTable.resize(size);
  memset(&m_codeTable[0], 0, size * sizeof(m_codeTable[0]));

  for (int i = i0; i < i1; i++)
  {
    int k = GetIndexWrapAround(i, size);
    m_codeTable[k].first = (short)dataVec[i - i0];
  }

  if (!BitUnStuffCodes(&ptr, i0, i1))    // unstuff the codes
    return false;

  *ppByte = ptr;
  return true;
}

// -------------------------------------------------------------------------- ;

bool Huffman::BuildTreeFromCodes(int& numBitsLUT)
{
  int i0, i1, maxLen;
  if (!GetRange(i0, i1, maxLen))
    return false;

  // build decode LUT using max of 12 bits
  int size = (int)m_codeTable.size();

  bool bNeedTree = maxLen > m_maxNumBitsLUT;
  numBitsLUT = min(maxLen, m_maxNumBitsLUT);

  m_decodeLUT.clear();
  m_decodeLUT.assign(1 << numBitsLUT, pair<short, short>((short)-1, (short)-1));

  for (int i = i0; i < i1; i++)
  {
    int k = GetIndexWrapAround(i, size);
    int len = m_codeTable[k].first;
    if (len > 0 && len <= numBitsLUT)
    {
      int code = m_codeTable[k].second << (numBitsLUT - len);
      pair<short, short> entry((short)len, (short)k);
      int numEntries = 1 << (numBitsLUT - len);
      for (int j = 0; j < numEntries; j++)
        m_decodeLUT[code | j] = entry;    // add the duplicates
    }
  }

  if (!bNeedTree)    // decode LUT covers it all, no tree needed
    return true;

  int numNodesCreated = 1;
  Node emptyNode((short)-1, 0);

  if (!m_root)
    m_root = new Node(emptyNode);

  for (int i = i0; i < i1; i++)
  {
    int k = GetIndexWrapAround(i, size);
    int len = m_codeTable[k].first;
    if (len > 0 && len > numBitsLUT)    // add only codes not in the decode LUT
    {
      unsigned int code = m_codeTable[k].second;
      Node* node = m_root;
      int j = len;
      while (--j >= 0)    // go over the bits
      {
        if (code & (1 << j))
        {
          if (!node->child1)
          {
            node->child1 = new Node(emptyNode);
            numNodesCreated++;
          }
          node = node->child1;
        }
        else
        {
          if (!node->child0)
          {
            node->child0 = new Node(emptyNode);
            numNodesCreated++;
          }
          node = node->child0;
        }

        if (j == 0)    // last bit, leaf node
        {
          node->value = (short)k;    // set the value
        }
      }
    }
  }

  return true;
}

// -------------------------------------------------------------------------- ;

void Huffman::Clear()
{
  m_codeTable.clear();
  m_decodeLUT.clear();
  if (m_root)
  {
    int n = 0;
    m_root->FreeTree(n);
    delete m_root;
    m_root = 0;
  }
}

// -------------------------------------------------------------------------- ;
// -------------------------------------------------------------------------- ;

bool Huffman::ComputeNumBytesCodeTable(int& numBytes) const
{
  int i0, i1, maxLen;
  if (!GetRange(i0, i1, maxLen))
    return false;

  int size = (int)m_codeTable.size();
  int sum = 0;
  for (int i = i0; i < i1; i++)
  {
    int k = GetIndexWrapAround(i, size);
    sum += m_codeTable[k].first;
  }

  numBytes = 4 * sizeof(int);    // version, size, first bin, (last + 1) bin

  BitStuffer2 bitStuffer2;
  numBytes += bitStuffer2.ComputeNumBytesNeededSimple((unsigned int)(i1 - i0), (unsigned int)maxLen);    // code lengths
  int numUInts = (((sum + 7) >> 3) + 3) >> 2;
  numBytes += 4 * numUInts;    // byte array with the codes bit stuffed

  return true;
}

// -------------------------------------------------------------------------- ;

bool Huffman::GetRange(int& i0, int& i1, int& maxCodeLength) const
{
  if (m_codeTable.empty() || m_codeTable.size() >= m_maxHistoSize)
    return false;

  // first, check for peak somewhere in the middle with 0 stretches left and right
  int size = (int)m_codeTable.size();
  int i = 0;
  while (i < size && m_codeTable[i].first == 0) i++;
  i0 = i;
  i = size - 1;
  while (i >= 0 && m_codeTable[i].first == 0) i--;
  i1 = i + 1;    // exclusive

  if (i1 <= i0)
    return false;

  // second, cover the common case that the peak is close to 0
  pair<int, int> segm(0, 0);
  int j = 0;
  while (j < size)    // find the largest stretch of 0's, if any
  {
    while (j < size && m_codeTable[j].first > 0) j++;
    int k0 = j;
    while (j < size && m_codeTable[j].first == 0) j++;
    int k1 = j;

    if (k1 - k0 > segm.second)
      segm = pair<int, int>(k0, k1 - k0);
  }

  if (size - segm.second < i1 - i0)
  {
    i0 = segm.first + segm.second;
    i1 = segm.first + size;    // do wrap around
  }

  if (i1 <= i0)
    return false;

  int maxLen = 0;
  for (int i = i0; i < i1; i++)
  {
    int k = GetIndexWrapAround(i, size);
    int len = m_codeTable[k].first;
    maxLen = max(maxLen, len);
  }

  if (maxLen <= 0 || maxLen > 32)
    return false;

  maxCodeLength = maxLen;
  return true;
}

// -------------------------------------------------------------------------- ;

bool Huffman::BitStuffCodes(Byte** ppByte, int i0, int i1) const
{
  if (!ppByte)
    return false;

  unsigned int* arr = (unsigned int*)(*ppByte);
  unsigned int* dstPtr = arr;
  int size = (int)m_codeTable.size();
  int bitPos = 0;

  for (int i = i0; i < i1; i++)
  {
    int k = GetIndexWrapAround(i, size);
    int len = m_codeTable[k].first;
    if (len > 0)
    {
      unsigned int val = m_codeTable[k].second;

      if (32 - bitPos >= len)
      {
        if (bitPos == 0)
          *dstPtr = 0;

        *dstPtr |= val << (32 - bitPos - len);
        bitPos += len;
        if (bitPos == 32)
        {
          bitPos = 0;
          dstPtr++;
        }
      }
      else
      {
        bitPos += len - 32;
        *dstPtr++ |= val >> bitPos;
        *dstPtr = val << (32 - bitPos);
      }
    }
  }

  size_t numUInts = dstPtr - arr + (bitPos > 0 ? 1 : 0);
  *ppByte += numUInts * sizeof(unsigned int);
  return true;
}

// -------------------------------------------------------------------------- ;

bool Huffman::BitUnStuffCodes(const Byte** ppByte, int i0, int i1)
{
  if (!ppByte || !(*ppByte))
    return false;

  const unsigned int* arr = (const unsigned int*)(*ppByte);
  const unsigned int* srcPtr = arr;
  int size = (int)m_codeTable.size();
  int bitPos = 0;

  for (int i = i0; i < i1; i++)
  {
    int k = GetIndexWrapAround(i, size);
    int len = m_codeTable[k].first;
    if (len > 0)
    {
      m_codeTable[k].second = ((*srcPtr) << bitPos) >> (32 - len);

      if (32 - bitPos >= len)
      {
        bitPos += len;
        if (bitPos == 32)
        {
          bitPos = 0;
          srcPtr++;
        }
      }
      else
      {
        bitPos += len - 32;
        srcPtr++;
        m_codeTable[k].second |= (*srcPtr) >> (32 - bitPos);
      }
    }
  }

  size_t numUInts = srcPtr - arr + (bitPos > 0 ? 1 : 0);
  *ppByte += numUInts * sizeof(unsigned int);
  return true;
}

// -------------------------------------------------------------------------- ;
