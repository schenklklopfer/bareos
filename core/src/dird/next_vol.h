/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2018-2018 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#ifndef BAREOS_DIRD_NEXT_VOL_H_
#define BAREOS_DIRD_NEXT_VOL_H_

void SetStorageidInMr(StorageResource *store, MediaDbRecord *mr);
int FindNextVolumeForAppend(JobControlRecord *jcr, MediaDbRecord *mr, int index,
                                const char *unwanted_volumes, bool create, bool purge);
bool HasVolumeExpired(JobControlRecord *jcr, MediaDbRecord *mr);
void CheckIfVolumeValidOrRecyclable(JobControlRecord *jcr, MediaDbRecord *mr, const char **reason);
bool GetScratchVolume(JobControlRecord *jcr, bool InChanger, MediaDbRecord *mr, StorageResource *store);

#endif // BAREOS_DIRD_NEXT_VOL_H_
