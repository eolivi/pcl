/*
 * Software License Agreement (BSD License)
 *
 *  Point Cloud Library (PCL) - www.pointclouds.org
 *  Copyright (c) 2010-2012, Willow Garage, Inc.
 *  
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *
 */

#ifndef PCL_RECOGNITION_OBJ_REC_RANSAC_H_
#define PCL_RECOGNITION_OBJ_REC_RANSAC_H_

#include <pcl/recognition/ransac_based/model_library.h>
#include <pcl/recognition/ransac_based/orr_octree.h>
#include <pcl/recognition/ransac_based/orr_octree_zprojection.h>
#include <pcl/recognition/ransac_based/auxiliary.h>
#include <pcl/pcl_exports.h>
#include <pcl/point_cloud.h>
#include <cmath>
#include <string>
#include <vector>
#include <list>

#define OBJ_REC_RANSAC_VERBOSE
#define OBJ_REC_RANSAC_TEST

namespace pcl
{
  namespace recognition
  {
    class ORRGraph;

    /** \brief This is a RANSAC-based 3D object recognition method. Do the following to use it: (i) call addModel() k times with k different models
      * representing the objects to be recognized and (ii) call recognize() with the 3D scene in which the objects should be recognized. Recognition means both
      * object identification and pose (position + orientation) estimation. Check the method descriptions for more details.
      *
      * \note If you use this code in any academic work, please cite:
      *
      *   - Chavdar Papazov, Sami Haddadin, Sven Parusel, Kai Krieger and Darius Burschka.
      *     Rigid 3D geometry matching for grasping of known objects in cluttered scenes.
      *     The International Journal of Robotics Research 2012. DOI: 10.1177/0278364911436019
      *
      *   - Chavdar Papazov and Darius Burschka.
      *     An Efficient RANSAC for 3D Object Recognition in Noisy and Occluded Scenes.
      *     In Proceedings of the 10th Asian Conference on Computer Vision (ACCV'10),
      *     November 2010.
      *
      *
      * \author Chavdar Papazov
      * \ingroup recognition
      */
    class PCL_EXPORTS ObjRecRANSAC
    {
      public:
        typedef ModelLibrary::PointCloudIn PointCloudIn;
        typedef ModelLibrary::PointCloudN PointCloudN;

        /** \brief This is an output item of the ObjRecRANSAC::recognize() method. It contains the recognized model, its name (the ones passed to
          * ObjRecRANSAC::addModel()), the rigid transform which aligns the model with the input scene and the match confidence which is a number
          * in the interval (0, 1] which gives the fraction of the model surface area matched to the scene. E.g., a match confidence of 0.3 means
          * that 30% of the object surface area was matched to the scene points. If the scene is represented by a single range image, the match
          * confidence can not be greater than 0.5 since the range scanner sees only one side of each object.
          */
        class Output
        {
          public:
            Output (const std::string& object_name, const float rigid_transform[12], float match_confidence, void* user_data) :
              object_name_ (object_name),
              match_confidence_ (match_confidence),
              user_data_ (user_data)
            {
              for ( int i = 0 ; i < 12 ; ++i )
                rigid_transform_[i] = rigid_transform[i];
            }
            virtual ~Output (){}

          public:
            std::string object_name_;
            float rigid_transform_[12];
            float match_confidence_;
            void* user_data_;
        };

    	class OrientedPointPair
    	{
          public:
            OrientedPointPair (const float *p1, const float *n1, const float *p2, const float *n2)
            : p1_ (p1), n1_ (n1), p2_ (p2), n2_ (n2)
            {
            }

            virtual ~OrientedPointPair (){}

          public:
            const float *p1_, *n1_, *p2_, *n2_;
    	};

    	class Hypothesis
    	{
          public:
            Hypothesis (const ModelLibrary::Model& obj_model)
             : match_confidence_ (-1.0f),
               obj_model_ (obj_model)
            {}
            virtual ~Hypothesis (){}

          public:
            float rigid_transform_[12];
            float match_confidence_;
            const ModelLibrary::Model& obj_model_;
            std::set<int> explained_pixels_;
#ifdef OBJ_REC_RANSAC_TEST
            int rot_3dId_[3], t_3dId_[3];
#endif
    	};

        class RotationSpace
        {
          public:
            class Cell
            {
            public:
              class Entry
              {
                public:
                  Entry ()
                  : num_transforms_ (0)
                  {
                    aux::set3 (axis_angle_, 0.0f);
                    aux::set3 (translation_, 0.0f);
                  }

                  Entry (const Entry& src)
                  : num_transforms_ (src.num_transforms_)
                  {
                    aux::copy3 (src.axis_angle_, this->axis_angle_);
                    aux::copy3 (src.translation_, this->translation_);
                  }

                  inline void
                  addRigidTransform (const float axis_angle[3], const float translation[3])
                  {
                    aux::add3 (this->axis_angle_, axis_angle);
                    aux::add3 (this->translation_, translation);
                    ++num_transforms_;
                  }

                  inline void
                  computeAverageRigidTransform ()
                  {
                    if ( num_transforms_ < 2 )
                      return;

                    float factor = 1.0f/static_cast<float> (num_transforms_);
                    aux::mult3 (this->axis_angle_, factor);
                    aux::mult3 (this->translation_, factor);
                    num_transforms_ = 1;
                  }

                  inline const float*
                  getAxisAngle () const
                  {
                    return (axis_angle_);
                  }

                  inline const float*
                  getTranslation () const
                  {
                    return (translation_);
                  }

                public:
                  float axis_angle_[3], translation_[3];
                  int num_transforms_;
              };// class Entry

            public:
              Cell (){}
              virtual ~Cell ()
              {
                model_to_entry_.clear ();
              }

              inline void
              addRigidTransform (const ModelLibrary::Model* model, const float axis_angle[3], const float translation[3])
              {
                model_to_entry_[model].addRigidTransform (axis_angle, translation);
              }

              inline void
              computeAverageRigidTransformInEntries (std::list<ObjRecRANSAC::Hypothesis*>& out, int& hypotheses_counter)
              {
                for ( std::map<const ModelLibrary::Model*,Entry>::iterator it = model_to_entry_.begin () ; it != model_to_entry_.end () ; ++it )
                {
                  // First, compute the average rigid transform (in the axis-angle representation)
                  it->second.computeAverageRigidTransform ();
                  // Now create a new hypothesis
                  ObjRecRANSAC::Hypothesis* new_hypo = new ObjRecRANSAC::Hypothesis(*it->first);
                  // Save the average rotation (in matrix form)
                  aux::axisAngleToRotationMatrix (it->second.getAxisAngle (), new_hypo->rigid_transform_);
                  // Save the average translation
                  aux::copy3 (it->second.getTranslation (), new_hypo->rigid_transform_ + 9);
                  // Save the new hypothesis
                  out.push_back (new_hypo);
#ifdef OBJ_REC_RANSAC_TEST
                  aux::copy3(t_3dId_, new_hypo->t_3dId_);
                  aux::copy3(rot_3dId_, new_hypo->rot_3dId_);
#endif
                }
                hypotheses_counter += static_cast<int> (model_to_entry_.size ());
              }

#ifdef OBJ_REC_RANSAC_TEST
            public:
              int rot_3dId_[3], t_3dId_[3];
#endif

            protected:
              std::map<const ModelLibrary::Model*,Entry> model_to_entry_;
            };// class Cell

          public:
            /** \brief We use the axis-angle representation for rotations. The axis is encoded in the vector
              * and the angle is its magnitude. This is represented in an octree with bounds [-pi, pi]^3. */
    		RotationSpace ()
    		{
              float min = -(AUX_PI_FLOAT + 0.000000001f), max = AUX_PI_FLOAT + 0.000000001f;
              float bounds[6] = {min, max, min, max, min, max};

              // Build the voxel structure
              rot_octree_.build (bounds, 6.0f*AUX_DEG_TO_RADIANS);
    		}

    		virtual ~RotationSpace ()
            {
              for ( std::list<RotationSpace::Cell*>::iterator it = full_cells_.begin () ; it != full_cells_.end () ; ++it )
                delete *it;
              full_cells_.clear ();
            }

    		inline bool
    		addRigidTransform (const ModelLibrary::Model* model, const float axis_angle[3], const float translation[3])
    		{
              ORROctree::Node* rot_leaf = rot_octree_.createLeaf (axis_angle[0], axis_angle[1], axis_angle[2]);

              if ( !rot_leaf )
              {
                const float *b = rot_octree_.getBounds ();
                printf ("WARNING in 'RotationSpace::%s()': the provided axis-angle input (%f, %f, %f) is "
                        "out of the rotation space bounds ([%f, %f], [%f, %f], [%f, %f]).\n",
                        __func__, axis_angle[0], axis_angle[1], axis_angle[2], b[0], b[1], b[2], b[3], b[4], b[5]);
                return (false);
              }

              RotationSpace::Cell* rot_cell;

              if ( !rot_leaf->getData ()->getUserData () )
              {
                rot_cell = new RotationSpace::Cell ();
                rot_leaf->getData ()->setUserData (rot_cell);
                full_cells_.push_back (rot_cell);
#ifdef OBJ_REC_RANSAC_TEST
                rot_leaf->getData ()->get3dId (rot_cell->rot_3dId_);
                aux::copy3(t_3dId_, rot_cell->t_3dId_);
#endif
              }
              else
                rot_cell = static_cast<RotationSpace::Cell*> (rot_leaf->getData ()->getUserData ());

              // Add the rigid transform to the cell
              rot_cell->addRigidTransform (model, axis_angle, translation);

              return (true);
            }

    		/** \brief For each full rotation space cell, the method computes the average rigid transform and creates and pushes back a new
    		  * hypothesis in 'out'. Increments 'hypotheses_counter' by the number of new created hypotheses. */
            inline void
            computeAverageRigidTransformInCells (std::list<ObjRecRANSAC::Hypothesis*>& out, int& hypotheses_counter)
            {
              for ( std::list<RotationSpace::Cell*>::iterator it = full_cells_.begin () ; it != full_cells_.end () ; ++it )
                (*it)->computeAverageRigidTransformInEntries (out, hypotheses_counter);
            }

          protected:
            ORROctree rot_octree_;
            std::list<RotationSpace::Cell*> full_cells_;
#ifdef OBJ_REC_RANSAC_TEST
          public:
            int t_3dId_[3];
#endif
        };// class RotationSpace

      public:
        /** \brief Constructor with some important parameters which can not be changed once an instance of that class is created.
          *
          * \param[in] pair_width should be roughly half the extent of the visible object part. This means, for each object point p there should be (at least)
          * one point q (from the same object) such that ||p - q|| <= pair_width. Tradeoff: smaller values allow for detection in more occluded scenes but lead
          * to more imprecise alignment. Bigger values lead to better alignment but require large visible object parts (i.e., less occlusion).
          *
          * \param[in] voxel_size is the size of the leafs of the octree, i.e., the "size" of the discretization. Tradeoff: High values lead to less
          * computation time but ignore object details. Small values allow to better distinguish between objects, but will introduce more holes in the resulting
          * "voxel-surface" (especially for a sparsely sampled scene). */
        ObjRecRANSAC (float pair_width, float voxel_size);
        virtual ~ObjRecRANSAC ()
        {
          this->clear ();
          this->clearTestData ();
        }

        /** \brief Removes all models from the model library and releases some memory dynamically allocated by this instance. */
        void
        inline clear()
        {
          model_library_.removeAllModels ();
          scene_octree_.clear ();
          scene_octree_proj_.clear ();
          sampled_oriented_point_pairs_.clear ();
          transform_octree_.clear ();
        }

        /** \brief This is a threshold. The larger the value the more point pairs will be considered as co-planar and will
          * be ignored in the off-line model pre-processing and in the online recognition phases. This makes sense only if
          * "ignore co-planar points" is on. Call this method before calling addModel. This method calls the corresponding
          * method of the model library. */
        inline void
        setMaxCoplanarityAngleDegrees (float max_coplanarity_angle_degrees)
        {
          max_coplanarity_angle_ = max_coplanarity_angle_degrees*AUX_DEG_TO_RADIANS;
          model_library_.setMaxCoplanarityAngleDegrees (max_coplanarity_angle_degrees);
        }

        inline void
        setSceneBoundsEnlargementFactor (float value)
        {
          scene_bounds_enlargement_factor_ = value;
        }

        /** \brief Default is on. This method calls the corresponding method of the model library. */
        inline void
        ignoreCoplanarPointPairsOn ()
        {
          ignore_coplanar_opps_ = true;
          model_library_.ignoreCoplanarPointPairsOn ();
        }

        /** \brief Default is on. This method calls the corresponding method of the model library. */
        inline void
        ignoreCoplanarPointPairsOff ()
        {
          ignore_coplanar_opps_ = false;
          model_library_.ignoreCoplanarPointPairsOff ();
        }

        /** \brief Add an object model to be recognized.
          *
          * \param[in] points are the object points.
          * \param[in] normals at each point.
          * \param[in] object_name is an identifier for the object. If that object is detected in the scene 'object_name'
          * is returned by the recognition method and you know which object has been detected. Note that 'object_name' has
          * to be unique!
          * \param[in] user_data is a pointer to some data (can be NULL)
          *
          * The method returns true if the model was successfully added to the model library and false otherwise (e.g., if 'object_name' is already in use).
          */
        inline bool
        addModel (const PointCloudIn& points, const PointCloudN& normals, const std::string& object_name, void* user_data = NULL)
        {
          return (model_library_.addModel (points, normals, object_name, user_data));
        }

        /** \brief This method performs the recognition of the models loaded to the model library with the method addModel().
          *
          * \param[in]  scene is the 3d scene in which the object should be recognized.
          * \param[in]  normals are the scene normals.
          * \param[out] recognized_objects is the list of output items each one containing the recognized model instance, its name, the aligning rigid transform
          * and the match confidence (see ObjRecRANSAC::Output for further explanations).
          * \param[in]  success_probability is the user-defined probability of detecting all objects in the scene.
          */
        void
        recognize (const PointCloudIn& scene, const PointCloudN& normals, std::list<ObjRecRANSAC::Output>& recognized_objects, double success_probability = 0.99);

        inline void
        enterTestModeSampleOPP ()
        {
          rec_mode_ = ObjRecRANSAC::SAMPLE_OPP;
        }

        inline void
        enterTestModeTestHypotheses ()
        {
          rec_mode_ = ObjRecRANSAC::TEST_HYPOTHESES;
        }

        inline void
        leaveTestMode ()
        {
          rec_mode_ = ObjRecRANSAC::FULL_RECOGNITION;
        }

        /** \brief This function is useful for testing purposes. It returns the oriented point pairs which were sampled from the
          * scene during the recognition process. Makes sense only if some of the testing modes are active. */
        inline const std::list<ObjRecRANSAC::OrientedPointPair>&
        getSampledOrientedPointPairs () const
        {
          return (sampled_oriented_point_pairs_);
        }

        /** \brief This function is useful for testing purposes. It returns the accepted hypotheses generated during the
          * recognition process. Makes sense only if some of the testing modes are active. */
        inline const std::vector<ObjRecRANSAC::Hypothesis*>&
        getAcceptedHypotheses () const
        {
          return (accepted_hypotheses_);
        }

        /** \brief This function is useful for testing purposes. It returns the accepted hypotheses generated during the
          * recognition process. Makes sense only if some of the testing modes are active. */
        inline void
        getAcceptedHypotheses (std::vector<ObjRecRANSAC::Hypothesis*>& out) const
        {
          out = accepted_hypotheses_;
        }

        /** \brief Returns the hash table in the model library. */
        inline const pcl::recognition::ModelLibrary::HashTable&
        getHashTable () const
        {
          return (model_library_.getHashTable ());
        }

        inline const ModelLibrary&
        getModelLibrary () const
        {
          return (model_library_);
        }

        inline const ModelLibrary::Model*
        getModel (const std::string& name) const
        {
          return (model_library_.getModel (name));
        }

        inline const ORROctree&
        getSceneOctree () const
        {
          return (scene_octree_);
        }

        inline const ORROctree&
        getTransformOctree () const
        {
          return (transform_octree_);
        }

        inline float
        getPairWidth () const
        {
          return pair_width_;
        }

      protected:
        enum Recognition_Mode {SAMPLE_OPP, TEST_HYPOTHESES, /*BUILD_CONFLICT_GRAPH,*/ FULL_RECOGNITION};

        friend class ModelLibrary;

        inline int
        computeNumberOfIterations (double success_probability)
        {
          // 'p_obj' is the probability that given that the first sample point belongs to an object,
          // the second sample point will belong to the same object
          const double p_obj = 0.25f;
          // old version: p = p_obj*relative_obj_size_*fraction_of_pairs_in_hash_table_;
          const double p = p_obj*relative_obj_size_;

          if ( 1.0 - p <= 0.0 )
            return 1;

          return static_cast<int> (log (1.0-success_probability)/log (1.0-p) + 1.0);
        }

        inline void
        clearTestData ()
        {
          sampled_oriented_point_pairs_.clear ();
          for ( std::vector<ObjRecRANSAC::Hypothesis*>::iterator it = accepted_hypotheses_.begin () ; it != accepted_hypotheses_.end () ; ++it )
            delete *it;
          accepted_hypotheses_.clear ();
        }

        void
        sampleOrientedPointPairs (int num_iterations, const std::vector<ORROctree::Node*>& full_scene_leaves, std::list<OrientedPointPair>& output);

        int
        generateHypotheses(const std::list<OrientedPointPair>& pairs, std::list<Hypothesis*>& out);

        /** \brief Groups repeating hypotheses in 'hypotheses'. Saves a representative for each group of repeating hypotheses
          * in 'out'. Returns the number of hypotheses after grouping. WARNING: the method deletes all hypotheses in the
          * provided input list 'hypotheses' and creates new ones and saves them in 'out'. Do not forget to destroy the memory
          * each item in 'out' is pointing to! */
        int
        groupHypotheses(std::list<Hypothesis*>& hypotheses, int num_hypotheses, std::list<Hypothesis*>& out);

        void
        testHypotheses (std::list<Hypothesis*>& hypotheses, int num_hypotheses, std::vector<Hypothesis*>& accepted_hypotheses);

        void
        buildConflictGraph (std::vector<Hypothesis*>& hypotheses, ORRGraph& graph);

        void
        filterWeakHypotheses (ORRGraph& graph, std::list<ObjRecRANSAC::Output>& recognized_objects);

    	/** \brief Computes the rigid transform that maps the line (a1, b1) to (a2, b2).
    	 * The computation is based on the corresponding points 'a1' <-> 'a2' and 'b1' <-> 'b2'
    	 * and the normals 'a1_n', 'b1_n', 'a2_n', and 'b2_n'. The result is saved in
    	 * 'rigid_transform' which is an array of length 12. The first 9 elements are the
    	 * rotational part (row major order) and the last 3 are the translation. */
        inline void
        computeRigidTransform(
          const float *a1, const float *a1_n, const float *b1, const float* b1_n,
          const float *a2, const float *a2_n, const float *b2, const float* b2_n,
          float* rigid_transform) const
        {
          // Some local variables
          float o1[3], o2[3], x1[3], x2[3], y1[3], y2[3], z1[3], z2[3], tmp1[3], tmp2[3], Ro1[3], invFrame1[3][3];

          // Compute the origins
          o1[0] = 0.5f*(a1[0] + b1[0]);
          o1[1] = 0.5f*(a1[1] + b1[1]);
          o1[2] = 0.5f*(a1[2] + b1[2]);

          o2[0] = 0.5f*(a2[0] + b2[0]);
          o2[1] = 0.5f*(a2[1] + b2[1]);
          o2[2] = 0.5f*(a2[2] + b2[2]);

          // Compute the x-axes
          aux::diff3 (b1, a1, x1); aux::normalize3 (x1);
          aux::diff3 (b2, a2, x2); aux::normalize3 (x2);
          // Compute the y-axes. First y-axis
          aux::projectOnPlane3 (a1_n, x1, tmp1); aux::normalize3 (tmp1);
          aux::projectOnPlane3 (b1_n, x1, tmp2); aux::normalize3 (tmp2);
          aux::sum3 (tmp1, tmp2, y1); aux::normalize3 (y1);
          // Second y-axis
          aux::projectOnPlane3 (a2_n, x2, tmp1); aux::normalize3 (tmp1);
          aux::projectOnPlane3 (b2_n, x2, tmp2); aux::normalize3 (tmp2);
          aux::sum3 (tmp1, tmp2, y2); aux::normalize3 (y2);
          // Compute the z-axes
          aux::cross3 (x1, y1, z1);
          aux::cross3 (x2, y2, z2);

          // 1. Invert the matrix [x1|y1|z1] (note that x1, y1, and z1 are treated as columns!)
          invFrame1[0][0] = x1[0]; invFrame1[0][1] = x1[1]; invFrame1[0][2] = x1[2];
          invFrame1[1][0] = y1[0]; invFrame1[1][1] = y1[1]; invFrame1[1][2] = y1[2];
          invFrame1[2][0] = z1[0]; invFrame1[2][1] = z1[1]; invFrame1[2][2] = z1[2];
          // 2. Compute the desired rotation as rigid_transform = [x2|y2|z2]*invFrame1
          aux::mult3x3 (x2, y2, z2, invFrame1, rigid_transform);

          // Construct the translation which is the difference between the rotated o1 and o2
          aux::mult3x3 (rigid_transform, o1, Ro1);
          rigid_transform[9]  = o2[0] - Ro1[0];
          rigid_transform[10] = o2[1] - Ro1[1];
          rigid_transform[11] = o2[2] - Ro1[2];
        }

        /** \brief Computes the signature of the oriented point pair ((p1, n1), (p2, n2)) consisting of the angles between
          * n1 and (p2-p1),
          * n2 and (p1-p2),
          * n1 and n2
          *
          * \param[out] signature is an array of three doubles saving the three angles in the order shown above. */
        static inline void
        compute_oriented_point_pair_signature (const float *p1, const float *n1, const float *p2, const float *n2, float signature[3])
        {
          // Get the line from p1 to p2
          float cl[3] = {p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2]};
          aux::normalize3 (cl);

          signature[0] = std::acos (aux::clamp (aux::dot3 (n1,cl), -1.0f, 1.0f)); cl[0] = -cl[0]; cl[1] = -cl[1]; cl[2] = -cl[2];
          signature[1] = std::acos (aux::clamp (aux::dot3 (n2,cl), -1.0f, 1.0f));
          signature[2] = std::acos (aux::clamp (aux::dot3 (n1,n2), -1.0f, 1.0f));
        }

      protected:
        // Parameters
        float pair_width_;
        float voxel_size_;
        float transform_octree_voxel_size_;
        float abs_zdist_thresh_;
        float relative_obj_size_;
        float visibility_;
        float relative_num_of_illegal_pts_;
        float intersection_fraction_;
        float max_coplanarity_angle_;
        float scene_bounds_enlargement_factor_;
        bool ignore_coplanar_opps_;

        ModelLibrary model_library_;
        ORROctree scene_octree_;
        ORROctree transform_octree_;
        ORROctreeZProjection scene_octree_proj_;

        std::list<OrientedPointPair> sampled_oriented_point_pairs_;
        std::vector<Hypothesis*> accepted_hypotheses_;
        Recognition_Mode rec_mode_;
    };
  } // namespace recognition
} // namespace pcl

#endif // PCL_RECOGNITION_OBJ_REC_RANSAC_H_
