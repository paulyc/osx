/*
 * JBoss, the OpenSource J2EE webOS
 *
 * Distributable under LGPL license.
 * See terms of license at gnu.org.
 */

package org.jboss.console.manager.interfaces;

/**
 * Standard menu entry used when right-clicking on an entry of the tree browser
 *
 * @see 
 *
 * @author  <a href="mailto:sacha.labourey@cogito-info.ch">Sacha Labourey</a>.
 * @version $Revision: 1.1.2.1 $
 *
 * <p><b>Revisions:</b>
 *
 * <p><b>December 16, 2002 Sacha Labourey:</b>
 * <ul>
 * <li> First implementation </li>
 * </ul>
 */

public interface SimpleTreeNodeMenuEntry
   extends TreeNodeMenuEntry
{
   public String getText ();
   public TreeAction getAction ();
}
