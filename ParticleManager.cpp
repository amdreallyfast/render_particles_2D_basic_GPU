#include "ParticleManager.h"

#include "glm/detail/func_geometric.hpp"    // glm::dot
#include "RandomToast.h"
#include "glload/include/glload/gl_4_4.h"


/*-----------------------------------------------------------------------------------------------
Description:
    An empty constructor.  It exists because I didn't know if anything had to go in it, and then
    I kept it around even when everything was moved to the Init(...) method.
Parameters: None
Returns:    None
Exception:  Safe
Creator:    John Cox (7-26-2016)
-----------------------------------------------------------------------------------------------*/
ParticleManager::ParticleManager()
{

}

/*-----------------------------------------------------------------------------------------------
Description:
    Calls Cleanup() in the event that the user forgot to call it themselves.
Parameters: None
Returns:    None
Exception:  Safe
Creator:    John Cox (7-30-2016)
-----------------------------------------------------------------------------------------------*/
ParticleManager::~ParticleManager()
{
    this->Cleanup();
}

/*-----------------------------------------------------------------------------------------------
Description:
    As the name suggests, this deletes the shader program, vertex buffer, and VAO associated 
    with this object.  Is called in the constructor in the event that someone forgot to call it 
    explicitly.  This method exists so that the user can reset it without deleting the actual 
    object (??why would you want to do this??) .
Parameters: None
Returns:    None
Exception:  Safe
Creator:    John Cox (7-26-2016)
-----------------------------------------------------------------------------------------------*/
void ParticleManager::Cleanup()
{
    glDeleteProgram(_programId);
    glDeleteProgram(_computeProgramId);
    glDeleteBuffers(1, &_shaderBufferId);
    glDeleteVertexArrays(1, &_vaoId);
}

/*-----------------------------------------------------------------------------------------------
Description:
    Records the program ID, the maximum number of particles to be emitted per frame, and the 
    emitter center.  Re-sizes the internal collection of particles to accomodate the specified 
    number.  Calculates the boundary at which particles become invalid based on the provided 
    radius.  Calculates the velocity delta from the provided min and max velocities.
Parameters: 
    programId       The shader program must be constructed prior to this.
    computeProgramId    Same issue.
    numParticles    The maximum number of particles that the manager has to work with.
    maxParticlesEmittedPerFrame     Self-explanatory.
    center          A 2D vector in window coordinates (X and Y bounded by [-1,+1]).
    radius          In window coords.  
    minVelocity     In window coords.
    maxVelocity     In window coords.
Returns:    None
Exception:  Safe
Creator:    John Cox (7-26-2016)
-----------------------------------------------------------------------------------------------*/
void ParticleManager::Init(unsigned int programId,
    unsigned int computeProgramId,
    unsigned int numParticles, 
    unsigned int maxParticlesEmittedPerFrame,
    const glm::vec2 center,
    float radius,
    float minVelocity,
    float maxVelocity)
{
    _programId = programId;
    _computeProgramId = computeProgramId;
    _allParticles.resize(numParticles);
    _sizeBytes = sizeof(Particle) * numParticles;
    _drawStyle = GL_POINTS;
    _maxParticlesEmittedPerFrame = maxParticlesEmittedPerFrame;
    _center = center;
    _radiusSqr = radius * radius;   // because only radius squared is used during update
    _velocityMin = minVelocity;
    _velocityDelta = maxVelocity - minVelocity;

    // start all particles at the emission orign
    for (size_t particleCount = 0; particleCount < _allParticles.size(); particleCount++)
    {
        // pointer arithmetic will do
        this->ResetParticle(_allParticles.data() + particleCount);
    }

    _unifLocDeltaTimeSec = glGetUniformLocation(_computeProgramId, "uDeltaTimeSec");
    _unifLocRadiusSqr = glGetUniformLocation(_computeProgramId, "uRadiusSqr");
    _unifLocEmitterCenter = glGetUniformLocation(_computeProgramId, "uEmitterCenter");
    _unifLocMaxParticlesEmittedPerFrame = glGetUniformLocation(_computeProgramId, "uMmaxParticlesEmittedPerFrame");
    _unifLocMaxParticleCount = glGetUniformLocation(_computeProgramId, "uMaxParticleCount");

    glUseProgram(_computeProgramId);
    
    glUniform1f(_unifLocRadiusSqr, _radiusSqr);
    glUniform1ui(_unifLocMaxParticlesEmittedPerFrame, maxParticlesEmittedPerFrame);
    glUniform1ui(_unifLocMaxParticleCount, numParticles);

    // feeding vectors into uniforms requires an array, or at least they need to be contiguous 
    // in memory, and I would rather explicitly spell out an array than assume the value order 
    // in a 3rd party struct
    float centerArr[4] = { center.x, center.y, 0.0f, 0.0f };
    glUniform2fv(_unifLocEmitterCenter, 1, centerArr);
    
    //??why are these work group counts all undefined??
    int workGroupCount[3];
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &workGroupCount[0]);
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 1, &workGroupCount[1]);
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 2, &workGroupCount[2]);
    printf("max global (total) work group counts: x = %d, y = %d, z = %d\n", workGroupCount[0], 
        workGroupCount[1], workGroupCount[2]);

    int workGroupSize[3];
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0, &workGroupCount[0]);
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 1, &workGroupCount[1]);
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 2, &workGroupCount[2]);
    printf("max global (total) work group sizes: x = %d, y = %d, z = %d\n", workGroupSize[0],
        workGroupSize[1], workGroupSize[2]);

    int workGroupInvocations = 0;
    // ??why is GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, which is in the lists at https://www.opengl.org/wiki/GLAPI/glGet, bute  undefined, but GL_MAX_COMPUTE_LOCAL_INVOCATIONS, which is not in the lists on that website, is defined? are they the same thing??
    glGetIntegerv(GL_MAX_COMPUTE_LOCAL_INVOCATIONS, &workGroupInvocations);
    printf("max local invocations = %d\n", workGroupInvocations);

    glUseProgram(0);

    // no program binding needed 
    // Note: Using a "shader storage buffer" because, unlike the vertex array buffer, this same buffer can be used for both the compute shader and the vertex shader.
    _shaderBufferId = 0;
    glGenBuffers(1, &_shaderBufferId);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, _shaderBufferId);
    glBufferData(GL_SHADER_STORAGE_BUFFER, _allParticles.size() * sizeof(Particle), _allParticles.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, _shaderBufferId);  // ??the hey does this do??

    // now set up the vertex array indices for the drawing shader
    // Note: MUST bind the program beforehand or else the VAO binding will blow up.  It won't 
    // spit out an error but will rather silently bind to whatever program is currently bound, 
    // even if it is the undefined program 0.
    glUseProgram(programId);
    glGenVertexArrays(1, &_vaoId);
    glBindVertexArray(_vaoId);
    glBindBuffer(GL_ARRAY_BUFFER, _shaderBufferId);
    // do NOT call glBufferData(...) because info was already loaded

    // position appears first in structure and so is attribute 0 
    // velocity appears second and is attribute 1
    // "is active" flag is third and is attribute 2
    unsigned int vertexArrayIndex = 0;
    unsigned int bufferStartOffset = 0;

    unsigned int bytesPerStep = sizeof(Particle);

    // position
    GLenum itemType = GL_FLOAT;
    unsigned int numItems = sizeof(Particle::_position) / sizeof(float);
    glEnableVertexAttribArray(vertexArrayIndex);
    glVertexAttribPointer(vertexArrayIndex, numItems, itemType, GL_FALSE, bytesPerStep, (void *)bufferStartOffset);

    // velocity
    itemType = GL_FLOAT;
    numItems = sizeof(Particle::_velocity) / sizeof(float);
    bufferStartOffset += sizeof(Particle::_position);
    vertexArrayIndex++;
    glEnableVertexAttribArray(vertexArrayIndex);
    glVertexAttribPointer(vertexArrayIndex, numItems, itemType, GL_FALSE, bytesPerStep, (void *)bufferStartOffset);

    // "is active" flag
    itemType = GL_INT;
    numItems = sizeof(Particle::_isActive) / sizeof(int);
    bufferStartOffset += sizeof(Particle::_velocity);
    vertexArrayIndex++;
    glEnableVertexAttribArray(vertexArrayIndex);
    glVertexAttribPointer(vertexArrayIndex, numItems, itemType, GL_FALSE, bytesPerStep, (void *)bufferStartOffset);

    // cleanup
    glBindVertexArray(0);   // unbind this BEFORE the array
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);    // always last
}

/*-----------------------------------------------------------------------------------------------
Description:
    Checks if each particle is out of bounds, and if so, resets it.  If the quota for emitted 
    particles hasn't been reached yet, then the particle is sent back out again.  Lastly, if the 
    particle is active, then its position is updated with its velocity and the provided delta 
    time.
Parameters:
    deltatimeSec        Self-explanatory
Returns:    None
Exception:  Safe
Creator:    John Cox (7-4-2016)
-----------------------------------------------------------------------------------------------*/
void ParticleManager::Update(float deltaTimeSec)
{
    // bind before attempting to send any uniforms or starting to compute stuff
    glUseProgram(_computeProgramId);
    glUniform1f(_unifLocDeltaTimeSec, deltaTimeSec);

    // the work groups specified here MUST (??you sure??) match the values specified by 
    // "local_size_x", "local_size_y", and "local_size_z" in the compute shader's input layout
    GLuint numWorkGroupsX = (_allParticles.size() / 256) + 1;
    GLuint numWorkGroupsY = 1;
    GLuint numWorkGroupsZ = 1;
    glDispatchCompute(numWorkGroupsX, numWorkGroupsY, numWorkGroupsZ);

    // tell the GPU:
    // (1) Accesses to the shader buffer after this call will reflect writes prior to the 
    // barrier.  This is only available in OpenGL 4.3 or higher.
    // (2) Vertex data sourced from buffer objects after the barrier will reflect data written 
    // by shaders prior to the barrier.  The affected buffer(s) is determined by the buffers 
    // that were bound for the vertex attributes.  In this case, that means GL_ARRAY_BUFFER.
    //glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    glUseProgram(0);
}

void ParticleManager::Render()
{
    glUseProgram(_programId);
    glBindVertexArray(_vaoId);
    glDrawArrays(_drawStyle, 0, _allParticles.size());
    glUseProgram(0);
}

/*-----------------------------------------------------------------------------------------------
Description:
    Checks if the provided particle has gone outside the circle.
Parameters:
    p   A const reference to a Particle object.
Returns:
    True if the particle's position is outside the circle's boundaries, otherwise false.
    Exception:  Safe
Creator:    John Cox (7-2-2016)
-----------------------------------------------------------------------------------------------*/
bool ParticleManager::OutOfBounds(const Particle &p) const
{
    glm::vec4 centerToParticle = p._position - glm::vec4(_center, 0.0f, 0.0f);
    float distSqr = glm::dot(centerToParticle, centerToParticle);
    if (distSqr > _radiusSqr)
    {
        return true;
    }
    else
    {
        return false;
    }

}

/*-----------------------------------------------------------------------------------------------
Description:
    Sets the given particle's starting position and velocity.  Does NOT alter the "is active"
    flag.  That flag is altered during Update(...).
Parameters:
    resetThis   Self-explanatory.
Returns:    None
Exception:  Safe
Creator:    John Cox (7-2-2016)
-----------------------------------------------------------------------------------------------*/
void ParticleManager::ResetParticle(Particle *resetThis) const
{
    // Note: The hard-coded mod100 is just to prevent the random axis magnitudes from 
    // getting too crazy different from each other.
    float newX = (float)(RandomPosAndNeg() % 100);
    float newY = (float)(RandomPosAndNeg() % 100);
    glm::vec2 randomVector = glm::normalize(glm::vec2(newX, newY));
    
    // hard-coded region of radius 0.1f in window space
    float radiusVariation = RandomOnRange0to1() * 0.1f; 

    resetThis->_position = glm::vec4(_center + (randomVector * radiusVariation), 0.0f, 0.0f);
    resetThis->_velocity = glm::vec4(this->GetNewVelocityVector(), 0.0f, 0.0f);
}

/*-----------------------------------------------------------------------------------------------
Description:
    Generates a new velocity vector between the previously provided minimum and maximum values 
    and in the provided direction.
Parameters: None
Returns:    
    A 2D vector whose magnitude is between the initialized "min" and "max" values and whose
    direction is random.
Exception:  Safe
Creator:    John Cox (7-2-2016)
-----------------------------------------------------------------------------------------------*/
glm::vec2 ParticleManager::GetNewVelocityVector() const
{
    // this demo particle "manager" emits in a circle, so get a random 2D direction
    // Note: The hard-coded mod100 is just to prevent the random axis magnitudes from 
    // getting too crazy different from each other.
    float newX = (float)(RandomPosAndNeg() % 100);
    float newY = (float)(RandomPosAndNeg() % 100);
    glm::vec2 randomVelocityVector = glm::normalize(glm::vec2(newX, newY));
    
    // randomize between the min and max velocities to get a little variation
    float velocityVariation = RandomOnRange0to1() * _velocityDelta;
    float velocityMagnitude = _velocityMin + velocityVariation;

    return randomVelocityVector * velocityMagnitude;
}
