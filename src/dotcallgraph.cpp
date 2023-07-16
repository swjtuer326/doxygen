/******************************************************************************
*
* Copyright (C) 1997-2020 by Dimitri van Heesch.
*
* Permission to use, copy, modify, and distribute this software and its
* documentation under the terms of the GNU General Public License is hereby
* granted. No representations are made about the suitability of this software
* for any purpose. It is provided "as is" without express or implied warranty.
* See the GNU General Public License for more details.
*
* Documents produced by Doxygen are derivative works derived from the
* input used in their production; they are not affected by this license.
*
*/

#include "dotcallgraph.h"

#include "dotnode.h"
#include "memberlist.h"
#include "config.h"
#include "util.h"
#include "portable.h"
#include "message.h"

using json = nlohmann::json;

// 可供后续精简图集使用
static QCString getUniqueId(const MemberDef *md)
{
  const MemberDef *def = md->memberDefinition();
  if (def==0) def = md;
  QCString result = def->getReference()+"$"+
         def->getOutputFileBase()+"#"+
         def->anchor();
  return result;
}

void DotCallGraph::buildGraph(DotNode *n,const MemberDef *md,int distance)
{
  auto refs = m_inverse ? md->getReferencedByMembers() : md->getReferencesMembers();
  for (const auto &rmd : refs)
  {
    if (rmd->isCallable())
    {
      QCString uniqueId = getUniqueId(rmd);
      auto it = m_usedNodes.find(uniqueId.str());
      if (it!=m_usedNodes.end()) // file is already a node in the graph
      {
        DotNode *bn = it->second;
        n->addChild(bn,EdgeInfo::Blue,EdgeInfo::Solid);
        bn->addParent(n);
        bn->setDistance(distance);
      }
      else
      {
        QCString name;
        QCString codeFragment;
        int startLine = rmd->getStartBodyLine();
        int endLine   = rmd->getEndBodyLine();
        readCodeFragment(rmd->getFileDef()->absFilePath(), startLine, endLine, codeFragment);


        if (Config_getBool(HIDE_SCOPE_NAMES))
        {
          name  = rmd->getOuterScope()==m_scope ?
            rmd->name() : rmd->qualifiedName();
          name = substitute(rmd->getFileDef()->relFilePath(), '.', '{') + "#" + name;
        }
        else
        {
          name = substitute(rmd->getFileDef()->relFilePath(), '.', '{') + "#" + rmd->qualifiedName();
        }
        name = name + "+" + std::to_string(startLine) + ":" + std::to_string(endLine);

        
        QCString tooltip = rmd->briefDescriptionAsTooltip();
        DotNode *bn = new DotNode(
            this,
            substitute(linkToText(rmd->getLanguage(),name,FALSE), '{', '.') + "\ncode:\n" + codeFragment,
            tooltip,
            uniqueId,
            0, //distance
            0,
            rmd
            );
        n->addChild(bn,EdgeInfo::Blue,EdgeInfo::Solid);
        bn->addParent(n);
        bn->setDistance(distance);
        m_usedNodes.insert(std::make_pair(uniqueId.str(),bn));
        // add a number that is stored as double (note the implicit conversion of j to an object)
        buildGraph(bn,rmd,distance+1);
      }
    }
  }
}

void DotCallGraph::determineVisibleNodes(DotNodeDeque &queue, int &maxNodes)
{
  while (!queue.empty() && maxNodes>0)
  {
    DotNode *n = queue.front();
    queue.pop_front();
    if (!n->isVisible() && n->distance()<=Config_getInt(MAX_DOT_GRAPH_DEPTH)) // not yet processed
    {
      n->markAsVisible();
      maxNodes--;
      // add direct children
      for (const auto &dn : n->children())
      {
        queue.push_back(dn);
      }
    }
  }
}

void DotCallGraph::determineTruncatedNodes(DotNodeDeque &queue)
{
  while (!queue.empty())
  {
    DotNode *n = queue.front();
    queue.pop_front();
    if (n->isVisible() && n->isTruncated()==DotNode::Unknown)
    {
      bool truncated = FALSE;
      for (const auto &dn : n->children())
      {
        if (!dn->isVisible())
          truncated = TRUE;
        else
          queue.push_back(dn);
      }
      n->markAsTruncated(truncated);
    }
  }
}

DotCallGraph::DotCallGraph(const MemberDef *md,bool inverse)
{
  m_inverse = inverse;
  m_diskName = md->getOutputFileBase()+"_"+md->anchor(); // 修改dot文件名称
  
  m_scope    = md->getOuterScope();
  QCString uniqueId = getUniqueId(md);
  QCString name;
  QCString codeFragment;
  int startLine = md->getStartBodyLine();
  int endLine   = md->getEndBodyLine();
  readCodeFragment(md->getFileDef()->absFilePath(), startLine, endLine, codeFragment);
  if (Config_getBool(HIDE_SCOPE_NAMES))
  {
    name = substitute(md->getFileDef()->relFilePath(), '.', '{') + "#" + md->name();
  }
  else
  {
    name = substitute(md->getFileDef()->relFilePath(), '.', '{') + "#" + md->qualifiedName();
  }
  name = name + "+" + std::to_string(md->getStartBodyLine()) + ":" + std::to_string(md->getEndBodyLine());

  QCString tooltip = md->briefDescriptionAsTooltip();
  m_startNode = new DotNode(this,
    substitute(linkToText(md->getLanguage(),name,FALSE), '{', '.') + "\ncode:\n" + codeFragment,
    tooltip,
    uniqueId,
    TRUE,     // root node
    0,
    md
  );
  m_startNode->setDistance(0);
  m_usedNodes.insert(std::make_pair(uniqueId.str(),m_startNode));
  buildGraph(m_startNode,md,1);

  int maxNodes = Config_getInt(DOT_GRAPH_MAX_NODES);
  DotNodeDeque openNodeQueue;
  openNodeQueue.push_back(m_startNode);
  determineVisibleNodes(openNodeQueue,maxNodes);
  openNodeQueue.clear();
  openNodeQueue.push_back(m_startNode);
  determineTruncatedNodes(openNodeQueue);
}

DotCallGraph::~DotCallGraph()
{
  DotNode::deleteNodes(m_startNode);
}

QCString DotCallGraph::getBaseName() const
{
  return m_diskName + (m_inverse ? "_icgraph" : "_cgraph");
}

void DotCallGraph::computeTheGraph()
{
  m_json = {{"node_num",0},{"edge_num",0},{"nodes",json::array()},{"edges",json::array()}};
  std::ofstream f = Portable::openOutputStream(absBaseName()+".json");
  msg("Patching output file %s\n",qPrint(absBaseName()+".json"));
  DotNodeRefVector writtenNodes;

  m_startNode->writeJson(m_json,writtenNodes);
  auto it = writtenNodes.begin();
  for (;it != writtenNodes.end();++it)
  {
    (*it)->resetWritten();
  }

  m_json["node_num"]=getNextNodeNumber()-1;
  m_json["edge_num"]=getNextEdgeNumber()-1;

  clearNextEdgeNumber();
  
  if (!f.is_open())
  {
    err("Could not open file %s for writing\n",qPrint(absBaseName()+".json"));
  }
  else
  {
    f << std::setw(4) << m_json << std::endl;
    f.close();
  }

  computeGraph(
    m_startNode,
    CallGraph,
    m_graphFormat,
    m_inverse ? "RL" : "LR",
    FALSE,
    m_inverse,
    m_startNode->label(),
    m_theGraph);
}

QCString DotCallGraph::getMapLabel() const
{
  return m_baseName;
}

QCString DotCallGraph::writeGraph(
        TextStream &out,
        GraphOutputFormat graphFormat,
        EmbeddedOutputFormat textFormat,
        const QCString &path,
        const QCString &fileName,
        const QCString &relPath,bool generateImageMap,
        int graphId)
{
  m_doNotAddImageToIndex = textFormat!=EOF_Html;

  return DotGraph::writeGraph(out, graphFormat, textFormat, path, fileName, relPath, generateImageMap, graphId);
}

bool DotCallGraph::isTrivial() const
{
  return m_startNode->children().empty();
}

bool DotCallGraph::isTooBig() const
{
  return numNodes()>=Config_getInt(DOT_GRAPH_MAX_NODES);
}

int DotCallGraph::numNodes() const
{
  return static_cast<int>(m_startNode->children().size());
}

bool DotCallGraph::isTrivial(const MemberDef *md,bool inverse)
{
  auto refs = inverse ? md->getReferencedByMembers() : md->getReferencesMembers();
  for (const auto &rmd : refs)
  {
    if (rmd->isCallable())
    {
      return FALSE;
    }
  }
  return TRUE;
}

