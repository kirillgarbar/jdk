/*
 * Copyright (c) 2023, Kirill Garbar, Red Hat, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package gc.msweep;

/**
 * @test TestMSweepEnabled
 * @summary Basic sanity test for MSweep
 * @library /test/lib
 *
 * @run main/othervm -Xmx256m
 *                   -XX:+UnlockExperimentalVMOptions -XX:+UseMSweepGC
 *                   gc.msweep.TestMSweepEnabled
 */

import java.lang.management.GarbageCollectorMXBean;
import java.lang.management.ManagementFactory;

public class TestMSweepEnabled {
  public static void main(String[] args) throws Exception {
    if (!isMSweepEnabled()) {
      throw new IllegalStateException("Debug builds should have MSweep enabled");
    }
  }

  public static boolean isMSweepEnabled() {
    for (GarbageCollectorMXBean bean : ManagementFactory.getGarbageCollectorMXBeans()) {
      if (bean.getName().contains("MSweep")) {
        return true;
      }
    }
    return false;
  }
}