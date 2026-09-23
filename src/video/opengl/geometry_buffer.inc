/* SPDX-License-Identifier: GPL-2.0-or-later
 * Reserve coordinate floats, not a fixed worst-case tessellation batch.
 * On failure the old buffers/capacity remain intact.
 */
static int YglReserveGeometry(YglProgram *program, unsigned needed) {
  const unsigned limit = 0x7fffffffU / (5 * sizeof(float));
  if (!needed || needed > limit || program->currentQuad < 0 ||
      program->maxQuad < 0 || program->currentQuad > program->maxQuad ||
      (unsigned)program->currentQuad > limit - needed)
    return -1;
  unsigned used = (unsigned)program->currentQuad;
  unsigned required = used + needed;
  if (required <= (unsigned)program->maxQuad && program->quads &&
      program->textcoords && program->vertexAttribute) return 0;
  unsigned capacity = (unsigned)program->maxQuad;
  if (capacity > limit) return -1;
  capacity = capacity > limit / 2 ? limit : capacity * 2;
  if (capacity < required) capacity = required;
  float *positions = malloc(capacity * sizeof(float));
  float *texcoords = malloc(capacity * 2 * sizeof(float));
  float *attributes = malloc(capacity * 2 * sizeof(float));
  if (!positions || !texcoords || !attributes ||
      (used && (!program->quads || !program->textcoords || !program->vertexAttribute))) {
    free(positions); free(texcoords); free(attributes);
    return -1;
  }
  if (used) {
    memcpy(positions, program->quads, used * sizeof(float));
    memcpy(texcoords, program->textcoords, used * 2 * sizeof(float));
    memcpy(attributes, program->vertexAttribute, used * 2 * sizeof(float));
  }
  free(program->quads); free(program->textcoords); free(program->vertexAttribute);
  program->quads = positions;
  program->textcoords = texcoords;
  program->vertexAttribute = attributes;
  program->maxQuad = (int)capacity;
  return 0;
}
