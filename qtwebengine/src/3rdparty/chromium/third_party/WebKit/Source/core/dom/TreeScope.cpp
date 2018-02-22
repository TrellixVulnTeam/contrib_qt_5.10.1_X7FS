/*
 * Copyright (C) 2011 Google Inc. All Rights Reserved.
 * Copyright (C) 2012 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "core/dom/TreeScope.h"

#include "core/HTMLNames.h"
#include "core/css/resolver/ScopedStyleResolver.h"
#include "core/dom/ContainerNode.h"
#include "core/dom/Document.h"
#include "core/dom/Element.h"
#include "core/dom/ElementShadow.h"
#include "core/dom/ElementTraversal.h"
#include "core/dom/IdTargetObserverRegistry.h"
#include "core/dom/NodeComputedStyle.h"
#include "core/dom/ShadowRoot.h"
#include "core/dom/StyleChangeReason.h"
#include "core/dom/TreeScopeAdopter.h"
#include "core/editing/DOMSelection.h"
#include "core/events/EventPath.h"
#include "core/frame/LocalFrame.h"
#include "core/frame/LocalFrameView.h"
#include "core/html/HTMLAnchorElement.h"
#include "core/html/HTMLFrameOwnerElement.h"
#include "core/html/HTMLMapElement.h"
#include "core/layout/HitTestResult.h"
#include "core/layout/api/LayoutViewItem.h"
#include "core/page/FocusController.h"
#include "core/page/Page.h"
#include "core/svg/SVGTreeScopeResources.h"
#include "platform/ScriptForbiddenScope.h"
#include "platform/wtf/Vector.h"

namespace blink {

using namespace HTMLNames;

TreeScope::TreeScope(ContainerNode& root_node, Document& document)
    : root_node_(&root_node),
      document_(&document),
      parent_tree_scope_(&document),
      id_target_observer_registry_(IdTargetObserverRegistry::Create()) {
  DCHECK_NE(root_node, document);
  root_node_->SetTreeScope(this);
}

TreeScope::TreeScope(Document& document)
    : root_node_(document),
      document_(&document),
      parent_tree_scope_(nullptr),
      id_target_observer_registry_(IdTargetObserverRegistry::Create()) {
  root_node_->SetTreeScope(this);
}

TreeScope::~TreeScope() {}

void TreeScope::ResetTreeScope() {
  selection_ = nullptr;
}

TreeScope* TreeScope::OlderShadowRootOrParentTreeScope() const {
  if (RootNode().IsShadowRoot()) {
    if (ShadowRoot* older_shadow_root =
            ToShadowRoot(RootNode()).OlderShadowRoot())
      return older_shadow_root;
  }
  return ParentTreeScope();
}

bool TreeScope::IsInclusiveOlderSiblingShadowRootOrAncestorTreeScopeOf(
    const TreeScope& scope) const {
  for (const TreeScope* current = &scope; current;
       current = current->OlderShadowRootOrParentTreeScope()) {
    if (current == this)
      return true;
  }
  return false;
}

void TreeScope::SetParentTreeScope(TreeScope& new_parent_scope) {
  // A document node cannot be re-parented.
  DCHECK(!RootNode().IsDocumentNode());

  parent_tree_scope_ = &new_parent_scope;
  SetDocument(new_parent_scope.GetDocument());
}

ScopedStyleResolver& TreeScope::EnsureScopedStyleResolver() {
  CHECK(this);
  if (!scoped_style_resolver_)
    scoped_style_resolver_ = ScopedStyleResolver::Create(*this);
  return *scoped_style_resolver_;
}

void TreeScope::ClearScopedStyleResolver() {
  scoped_style_resolver_.Clear();
}

Element* TreeScope::getElementById(const AtomicString& element_id) const {
  if (element_id.IsEmpty())
    return nullptr;
  if (!elements_by_id_)
    return nullptr;
  return elements_by_id_->GetElementById(element_id, *this);
}

const HeapVector<Member<Element>>& TreeScope::GetAllElementsById(
    const AtomicString& element_id) const {
  DEFINE_STATIC_LOCAL(HeapVector<Member<Element>>, empty_vector,
                      (new HeapVector<Member<Element>>));
  if (element_id.IsEmpty())
    return empty_vector;
  if (!elements_by_id_)
    return empty_vector;
  return elements_by_id_->GetAllElementsById(element_id, *this);
}

void TreeScope::AddElementById(const AtomicString& element_id,
                               Element* element) {
  if (!elements_by_id_)
    elements_by_id_ = TreeOrderedMap::Create();
  elements_by_id_->Add(element_id, element);
  id_target_observer_registry_->NotifyObservers(element_id);
}

void TreeScope::RemoveElementById(const AtomicString& element_id,
                                  Element* element) {
  if (!elements_by_id_)
    return;
  elements_by_id_->Remove(element_id, element);
  id_target_observer_registry_->NotifyObservers(element_id);
}

Node* TreeScope::AncestorInThisScope(Node* node) const {
  while (node) {
    if (node->GetTreeScope() == this)
      return node;
    if (!node->IsInShadowTree())
      return nullptr;

    node = node->OwnerShadowHost();
  }

  return nullptr;
}

void TreeScope::AddImageMap(HTMLMapElement* image_map) {
  const AtomicString& name = image_map->GetName();
  if (!name)
    return;
  if (!image_maps_by_name_)
    image_maps_by_name_ = TreeOrderedMap::Create();
  image_maps_by_name_->Add(name, image_map);
}

void TreeScope::RemoveImageMap(HTMLMapElement* image_map) {
  if (!image_maps_by_name_)
    return;
  const AtomicString& name = image_map->GetName();
  if (!name)
    return;
  image_maps_by_name_->Remove(name, image_map);
}

HTMLMapElement* TreeScope::GetImageMap(const String& url) const {
  if (url.IsNull())
    return nullptr;
  if (!image_maps_by_name_)
    return nullptr;
  size_t hash_pos = url.find('#');
  String name = hash_pos == kNotFound ? url : url.Substring(hash_pos + 1);
  return toHTMLMapElement(
      image_maps_by_name_->GetElementByMapName(AtomicString(name), *this));
}

static bool PointWithScrollAndZoomIfPossible(const Document& document,
                                             IntPoint& point) {
  LocalFrame* frame = document.GetFrame();
  if (!frame)
    return false;
  LocalFrameView* frame_view = frame->View();
  if (!frame_view)
    return false;

  FloatPoint point_in_document(point);
  point_in_document.Scale(frame->PageZoomFactor(), frame->PageZoomFactor());
  point_in_document.Move(frame_view->GetScrollOffset());
  IntPoint rounded_point_in_document = RoundedIntPoint(point_in_document);

  if (!frame_view->VisibleContentRect().Contains(rounded_point_in_document))
    return false;

  point = rounded_point_in_document;
  return true;
}

HitTestResult HitTestInDocument(const Document* document,
                                int x,
                                int y,
                                const HitTestRequest& request) {
  IntPoint hit_point(x, y);
  if (!PointWithScrollAndZoomIfPossible(*document, hit_point))
    return HitTestResult();

  if (!document->IsActive())
    return HitTestResult();

  HitTestResult result(request, hit_point);
  document->GetLayoutViewItem().HitTest(result);
  return result;
}

Element* TreeScope::ElementFromPoint(int x, int y) const {
  return HitTestPoint(x, y,
                      HitTestRequest::kReadOnly | HitTestRequest::kActive);
}

Element* TreeScope::HitTestPoint(int x,
                                 int y,
                                 const HitTestRequest& request) const {
  HitTestResult result =
      HitTestInDocument(&RootNode().GetDocument(), x, y, request);
  Node* node = result.InnerNode();
  if (!node || node->IsDocumentNode())
    return nullptr;
  if (node->IsPseudoElement() || node->IsTextNode())
    node = node->ParentOrShadowHostNode();
  DCHECK(!node || node->IsElementNode() || node->IsShadowRoot());
  node = AncestorInThisScope(node);
  if (!node || !node->IsElementNode())
    return nullptr;
  return ToElement(node);
}

HeapVector<Member<Element>> TreeScope::ElementsFromHitTestResult(
    HitTestResult& result) const {
  HeapVector<Member<Element>> elements;

  Node* last_node = nullptr;
  for (const auto rect_based_node : result.ListBasedTestResult()) {
    Node* node = rect_based_node.Get();
    if (!node || !node->IsElementNode() || node->IsDocumentNode())
      continue;

    if (node->IsPseudoElement() || node->IsTextNode())
      node = node->ParentOrShadowHostNode();
    node = AncestorInThisScope(node);

    // Prune duplicate entries. A pseduo ::before content above its parent
    // node should only result in a single entry.
    if (node == last_node)
      continue;

    if (node && node->IsElementNode()) {
      elements.push_back(ToElement(node));
      last_node = node;
    }
  }

  if (RootNode().IsDocumentNode()) {
    if (Element* root_element = ToDocument(RootNode()).documentElement()) {
      if (elements.IsEmpty() || elements.back() != root_element)
        elements.push_back(root_element);
    }
  }

  return elements;
}

HeapVector<Member<Element>> TreeScope::ElementsFromPoint(int x, int y) const {
  Document& document = RootNode().GetDocument();
  IntPoint hit_point(x, y);
  if (!PointWithScrollAndZoomIfPossible(document, hit_point))
    return HeapVector<Member<Element>>();

  HitTestRequest request(HitTestRequest::kReadOnly | HitTestRequest::kActive |
                         HitTestRequest::kListBased |
                         HitTestRequest::kPenetratingList);
  HitTestResult result(request, hit_point);
  document.GetLayoutViewItem().HitTest(result);

  return ElementsFromHitTestResult(result);
}

SVGTreeScopeResources& TreeScope::EnsureSVGTreeScopedResources() {
  if (!svg_tree_scoped_resources_)
    svg_tree_scoped_resources_ = new SVGTreeScopeResources(this);
  return *svg_tree_scoped_resources_;
}

DOMSelection* TreeScope::GetSelection() const {
  if (!RootNode().GetDocument().GetFrame())
    return nullptr;

  if (selection_)
    return selection_.Get();

  // FIXME: The correct selection in Shadow DOM requires that Position can have
  // a ShadowRoot as a container.  See
  // https://bugs.webkit.org/show_bug.cgi?id=82697
  selection_ = DOMSelection::Create(this);
  return selection_.Get();
}

Element* TreeScope::FindAnchor(const String& name) {
  if (name.IsEmpty())
    return nullptr;
  if (Element* element = getElementById(AtomicString(name)))
    return element;
  for (HTMLAnchorElement& anchor :
       Traversal<HTMLAnchorElement>::StartsAfter(RootNode())) {
    if (RootNode().GetDocument().InQuirksMode()) {
      // Quirks mode, case insensitive comparison of names.
      if (DeprecatedEqualIgnoringCase(anchor.GetName(), name))
        return &anchor;
    } else {
      // Strict mode, names need to match exactly.
      if (anchor.GetName() == name)
        return &anchor;
    }
  }
  return nullptr;
}

void TreeScope::AdoptIfNeeded(Node& node) {
  // Script is forbidden to protect against event handlers firing in the middle
  // of rescoping in |didMoveToNewDocument| callbacks. See
  // https://crbug.com/605766 and https://crbug.com/606651.
  ScriptForbiddenScope forbid_script;
  DCHECK(this);
  DCHECK(!node.IsDocumentNode());
  TreeScopeAdopter adopter(node, *this);
  if (adopter.NeedsScopeChange())
    adopter.Execute();
}

Element* TreeScope::Retarget(const Element& target) const {
  for (const Element* ancestor = &target; ancestor;
       ancestor = ancestor->OwnerShadowHost()) {
    if (this == ancestor->GetTreeScope())
      return const_cast<Element*>(ancestor);
  }
  return nullptr;
}

Element* TreeScope::AdjustedFocusedElement() const {
  Document& document = RootNode().GetDocument();
  Element* element = document.FocusedElement();
  if (!element && document.GetPage())
    element = document.GetPage()->GetFocusController().FocusedFrameOwnerElement(
        *document.GetFrame());
  if (!element)
    return nullptr;

  if (RootNode().IsInV1ShadowTree()) {
    if (Element* retargeted = Retarget(*element)) {
      return (this == &retargeted->GetTreeScope()) ? retargeted : nullptr;
    }
    return nullptr;
  }

  EventPath* event_path = new EventPath(*element);
  for (const auto& context : event_path->NodeEventContexts()) {
    if (context.GetNode() == RootNode()) {
      // context.target() is one of the followings:
      // - InsertionPoint
      // - shadow host
      // - Document::focusedElement()
      // So, it's safe to do toElement().
      return ToElement(context.Target()->ToNode());
    }
  }
  return nullptr;
}

Element* TreeScope::AdjustedElement(const Element& target) const {
  const Element* adjusted_target = &target;
  for (const Element* ancestor = &target; ancestor;
       ancestor = ancestor->OwnerShadowHost()) {
    // This adjustment is done only for V1 shadows, and is skipped for V0 or UA
    // shadows, because .pointerLockElement and .(webkit)fullscreenElement is
    // not available for non-V1 shadow roots.
    // TODO(kochi): Once V0 code is removed, use the same logic as
    // .activeElement for V1.
    if (ancestor->ShadowRootIfV1())
      adjusted_target = ancestor;
    if (this == ancestor->GetTreeScope())
      return const_cast<Element*>(adjusted_target);
  }
  return nullptr;
}

unsigned short TreeScope::ComparePosition(const TreeScope& other_scope) const {
  if (other_scope == this)
    return Node::kDocumentPositionEquivalent;

  HeapVector<Member<const TreeScope>, 16> chain1;
  HeapVector<Member<const TreeScope>, 16> chain2;
  const TreeScope* current;
  for (current = this; current; current = current->ParentTreeScope())
    chain1.push_back(current);
  for (current = &other_scope; current; current = current->ParentTreeScope())
    chain2.push_back(current);

  unsigned index1 = chain1.size();
  unsigned index2 = chain2.size();
  if (chain1[index1 - 1] != chain2[index2 - 1])
    return Node::kDocumentPositionDisconnected |
           Node::kDocumentPositionImplementationSpecific;

  for (unsigned i = std::min(index1, index2); i; --i) {
    const TreeScope* child1 = chain1[--index1];
    const TreeScope* child2 = chain2[--index2];
    if (child1 != child2) {
      Node* shadow_host1 = child1->RootNode().ParentOrShadowHostNode();
      Node* shadow_host2 = child2->RootNode().ParentOrShadowHostNode();
      if (shadow_host1 != shadow_host2)
        return shadow_host1->compareDocumentPosition(
            shadow_host2, Node::kTreatShadowTreesAsDisconnected);

      for (const ShadowRoot* child =
               ToShadowRoot(child2->RootNode()).OlderShadowRoot();
           child; child = child->OlderShadowRoot()) {
        if (child == child1)
          return Node::kDocumentPositionFollowing;
      }

      return Node::kDocumentPositionPreceding;
    }
  }

  // There was no difference between the two parent chains, i.e., one was a
  // subset of the other. The shorter chain is the ancestor.
  return index1 < index2 ? Node::kDocumentPositionFollowing |
                               Node::kDocumentPositionContainedBy
                         : Node::kDocumentPositionPreceding |
                               Node::kDocumentPositionContains;
}

const TreeScope* TreeScope::CommonAncestorTreeScope(
    const TreeScope& other) const {
  HeapVector<Member<const TreeScope>, 16> this_chain;
  for (const TreeScope* tree = this; tree; tree = tree->ParentTreeScope())
    this_chain.push_back(tree);

  HeapVector<Member<const TreeScope>, 16> other_chain;
  for (const TreeScope* tree = &other; tree; tree = tree->ParentTreeScope())
    other_chain.push_back(tree);

  // Keep popping out the last elements of these chains until a mismatched pair
  // is found. If |this| and |other| belong to different documents, null will be
  // returned.
  const TreeScope* last_ancestor = nullptr;
  while (!this_chain.IsEmpty() && !other_chain.IsEmpty() &&
         this_chain.back() == other_chain.back()) {
    last_ancestor = this_chain.back();
    this_chain.pop_back();
    other_chain.pop_back();
  }
  return last_ancestor;
}

TreeScope* TreeScope::CommonAncestorTreeScope(TreeScope& other) {
  return const_cast<TreeScope*>(
      static_cast<const TreeScope&>(*this).CommonAncestorTreeScope(other));
}

bool TreeScope::IsInclusiveAncestorOf(const TreeScope& scope) const {
  for (const TreeScope* current = &scope; current;
       current = current->ParentTreeScope()) {
    if (current == this)
      return true;
  }
  return false;
}

Element* TreeScope::GetElementByAccessKey(const String& key) const {
  if (key.IsEmpty())
    return nullptr;
  Element* result = nullptr;
  Node& root = RootNode();
  for (Element& element : ElementTraversal::DescendantsOf(root)) {
    if (DeprecatedEqualIgnoringCase(element.FastGetAttribute(accesskeyAttr),
                                    key))
      result = &element;
    for (ShadowRoot* shadow_root = element.YoungestShadowRoot(); shadow_root;
         shadow_root = shadow_root->OlderShadowRoot()) {
      if (Element* shadow_result = shadow_root->GetElementByAccessKey(key))
        result = shadow_result;
    }
  }
  return result;
}

void TreeScope::SetNeedsStyleRecalcForViewportUnits() {
  for (Element* element = ElementTraversal::FirstWithin(RootNode()); element;
       element = ElementTraversal::NextIncludingPseudo(*element)) {
    for (ShadowRoot* root = element->YoungestShadowRoot(); root;
         root = root->OlderShadowRoot())
      root->SetNeedsStyleRecalcForViewportUnits();
    const ComputedStyle* style = element->GetComputedStyle();
    if (style && style->HasViewportUnits())
      element->SetNeedsStyleRecalc(kLocalStyleChange,
                                   StyleChangeReasonForTracing::Create(
                                       StyleChangeReason::kViewportUnits));
  }
}

DEFINE_TRACE(TreeScope) {
  visitor->Trace(root_node_);
  visitor->Trace(document_);
  visitor->Trace(parent_tree_scope_);
  visitor->Trace(id_target_observer_registry_);
  visitor->Trace(selection_);
  visitor->Trace(elements_by_id_);
  visitor->Trace(image_maps_by_name_);
  visitor->Trace(scoped_style_resolver_);
  visitor->Trace(radio_button_group_scope_);
  visitor->Trace(svg_tree_scoped_resources_);
}

}  // namespace blink